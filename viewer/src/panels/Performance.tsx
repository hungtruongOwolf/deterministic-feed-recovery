// What it costs, and what that does not tell you.
//
// This section exists because the project aimed at low-latency work and had no performance number anywhere in
// it for months. The build preset called `bench` turned assertions off and measured nothing.
//
// The awkward honesty this panel has to carry
// -------------------------------------------
// Everything else on this page is computed in the reader's browser, on their seed. These figures are not: they
// were measured natively on the author's laptop and are read from a committed file. WebAssembly cannot produce
// them — its clock is coarse and its code is not the code that would ship — so a "measured in your browser"
// figure here would be a different quantity wearing the same label. The panel says so at the top rather than
// letting somebody assume otherwise, because the rest of the page has earned that assumption.

import {
  assertionCosts,
  nanos,
  rate,
  type Measurement,
  type Performance as PerfData,
  type PerfLimit,
} from "../model/perf";

interface Props {
  readonly perf: PerfData;
}

export function Performance({ perf }: Props) {
  const costs = assertionCosts(perf);
  const significant = costs.filter((c) => c.significant);
  const hot = perf.shipping.measurements.find((m) => m.name.startsWith("ingest a packet"));
  const batched = perf.handoff.measurements.find((m) => m.name.includes("batches"));

  return (
    <section className="perf">
      <header className="perf__head">
        <h2 className="perf__title">What it costs</h2>
        <p className="perf__lede">
          The recovery path is code that only runs when something has already gone wrong, which is exactly the
          code nobody benchmarks. These are its costs: nanoseconds per operation on one core, with the tail
          shown rather than averaged away.
        </p>
        <p className="perf__provenance">
          <strong>Measured natively, not in your browser.</strong> Everything else on this page runs in
          WebAssembly on your machine; these figures cannot. WebAssembly's clock is too coarse to time a
          two-nanosecond operation and its output is not the code that would ship, so a browser figure here
          would be a different quantity wearing the same label. Apple M-series laptop, <code>-O3 -flto</code>,{" "}
          {perf.shipping.rounds ?? 1} rounds, minimum per figure — noise on a benchmark only ever adds time.
        </p>
      </header>

      {/* The two figures worth putting in large type, because they are the two a reader remembers. */}
      <div className="perf__headline">
        <Headline
          value={hot === undefined ? "—" : nanos(hot.best_ns)}
          label="to take in one packet"
          note={hot === undefined ? "" : `${rate(hot.per_second)} · ${hot.unit}`}
        />
        <Headline
          value={String(perf.shipping.allocations_after_init)}
          label="allocations after start-up"
          note="counted by replacing global operator new across a whole recovery run"
          good={perf.shipping.allocations_after_init === 0}
        />
        <Headline
          value={batched === undefined ? "—" : nanos(batched.ns_per_message)}
          label="to hand a message to another core"
          note={batched === undefined ? "" : `${rate(batched.messages_per_second)} · ${batched.note}`}
        />
      </div>

      <h3 className="perf__sub mono">THE RECOVERY PATH, PER OPERATION</h3>
      <table className="perf__table">
        <thead>
          <tr>
            <th>operation</th>
            <th>one is</th>
            <th className="perf__num">best</th>
            <th className="perf__num">p50</th>
            <th className="perf__num">p99</th>
            <th className="perf__num">rate</th>
          </tr>
        </thead>
        <tbody>
          {perf.shipping.measurements.map((m: Measurement) => (
            <tr key={m.name}>
              <td>{m.name}</td>
              <td className="perf__unit">{m.unit}</td>
              <td className="perf__num mono">{nanos(m.best_ns)}</td>
              <td className="perf__num mono">{nanos(m.p50_ns)}</td>
              <td className="perf__num mono">{nanos(m.p99_ns)}</td>
              <td className="perf__num mono">{rate(m.per_second)}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <p className="perf__caveat">
        The percentiles are over <em>batch means</em>, not over individual operations: a{" "}
        <code>steady_clock</code> tick is tens of nanoseconds and several of these operations cost less than
        that, so each sample times a batch and divides. That means a p99 here cannot show one ten-microsecond
        stall inside a batch of a thousand — it shows that some batches ran consistently slower than others,
        which is what scheduler noise looks like. A true per-operation tail needs hardware timestamps.
      </p>

      <h3 className="perf__sub mono">WHAT THE PARANOID ASSERTIONS COST</h3>
      <p className="perf__prose">
        The library can be built with bounds checks on every field read. Pricing them by comparing the{" "}
        <code>dev</code> and <code>bench</code> presets is the obvious move and it is wrong — one is{" "}
        <code>Debug</code> and the other is <code>Release</code>, so it prices two variables and reports the
        sum as one. It gave a 55× ratio, which is a real number about a configuration nobody would ship.
        Holding <code>-O3</code> fixed and moving only the assertion level gives a more interesting answer:
      </p>
      <div className="perf__costs">
        {costs.map((c) => (
          <div key={c.name} className={`cost ${c.significant ? "is-real" : "is-noise"}`}>
            <span className="cost__ratio mono">
              {c.significant ? `${c.ratio.toFixed(1)}×` : "—"}
            </span>
            <span className="cost__name">{c.name}</span>
            <span className="cost__note">
              {c.significant ? "measurably slower with assertions on" : "difference is inside the noise"}
            </span>
          </div>
        ))}
      </div>
      <p className="perf__prose">
        <strong>
          So the paranoid assertions can stay on in production on this path — they cost nothing measurable
          where the work is.
        </strong>{" "}
        They cost {significant.length > 0 ? `${significant[0]!.ratio.toFixed(1)}×` : "several times"} on the
        tightest operation, which is almost entirely bounds checks once the checks are on, and nothing
        detectable on the paths that dominate an ingest — arbitration, gap arithmetic, watermark bookkeeping —
        because that is work the assertions never touch. That conclusion is the opposite of the one the 55×
        number pointed at, and no design document could have produced it.
      </p>

      <h3 className="perf__sub mono">ACROSS A THREAD BOUNDARY</h3>
      <p className="perf__prose">
        The recovery core is single-threaded and stays that way: determinism is what the project rests on, and
        a multi-threaded core would make the thread interleaving part of the input, leaving nothing to
        reproduce from a seed. So the concurrency sits at one seam, where real feed handlers put it — a
        lock-free ring between the thread that owns the protocol and the thread that owns the strategy. A full
        ring <em>refuses and counts it</em> rather than overwriting the oldest record, because overwriting
        turns a known backlog into a silent hole.
      </p>
      <table className="perf__table">
        <thead>
          <tr>
            <th>hand-off</th>
            <th>which is</th>
            <th className="perf__num">per message</th>
            <th className="perf__num">rate</th>
            <th className="perf__num">refused</th>
          </tr>
        </thead>
        <tbody>
          {perf.handoff.measurements.map((h) => (
            <tr key={h.name} className={h.refused > 0 ? "is-refusing" : ""}>
              <td>{h.name}</td>
              <td className="perf__unit">{h.note}</td>
              <td className="perf__num mono">{nanos(h.ns_per_message)}</td>
              <td className="perf__num mono">{rate(h.messages_per_second)}</td>
              <td className="perf__num mono">{h.refused > 0 ? h.refused.toLocaleString() : "—"}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <p className="perf__caveat">
        Two of those rows changed my mind. Cache-line padding on the two indices is worth{" "}
        <strong>10–25%</strong>, not the order of magnitude the received wisdom claims — and the first attempt
        at measuring it found the unpadded ring <em>faster</em>, because copying a 272-byte record costs more
        than the cache line the indices fight over, so the fight hides underneath it. Batching the drain is the
        bigger win, at about 2× per message, because one core-to-core cache-line transfer serves the whole
        batch. The row that refuses is the design decision working: the producer was turned away ten million
        times and the ring can say so.
      </p>

      <h3 className="perf__sub mono">WHAT IS NOT MEASURED, AND WHY</h3>
      <div className="perf__limits">
        {[...perf.shipping.limits, ...perf.handoff.limits]
          .filter((l: PerfLimit) => l.status !== "measured")
          .map((l: PerfLimit) => (
            <div key={l.claim} className="perf__limit">
              <span className="perf__limit-claim">{l.claim}</span>
              <span className="perf__limit-note">{l.note}</span>
            </div>
          ))}
      </div>
      <p className="perf__caveat">
        No tick-to-trade figure, no NIC-to-NIC figure, no wire latency of any kind. All of the above is CPU
        time inside one process with no network in the loop; measuring wire latency needs NIC hardware
        timestamping and PMU counters that a laptop and a cloud VM do not have. No number is given rather than
        a number with nothing behind it — the same rule the run traces follow.
      </p>
    </section>
  );
}

function Headline({
  value,
  label,
  note,
  good,
}: {
  readonly value: string;
  readonly label: string;
  readonly note: string;
  readonly good?: boolean;
}) {
  return (
    <div className={`headline ${good === true ? "is-good" : ""}`}>
      <span className="headline__value">{value}</span>
      <span className="headline__label">{label}</span>
      <span className="headline__note">{note}</span>
    </div>
  );
}

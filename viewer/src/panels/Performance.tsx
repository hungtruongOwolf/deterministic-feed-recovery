// What it costs: three figures, two tables, and the essays kept in docs/ where they belong.
//
// This panel was 538 words — the single largest block of prose on the page. It argued the whole benchmark
// method: batch means, the noise floor, why comparing the dev and bench presets is wrong, the three measurement
// bugs, what padding is worth. Every word of that is worth having and none of it is worth putting between a
// reader and the numbers.
//
// So it lives in docs/BENCHMARKS.md and docs/CONCURRENCY.md, which are linked, and the page carries the two
// sentences that change how a figure should be read: these were measured natively rather than in your browser,
// and no wire latency is claimed.

import {
  assertionCosts,
  nanos,
  rate,
  type Measurement,
  type Performance as PerfData,
} from "../model/perf";
import { type LiveRate } from "../model/here";
import { Disclosure } from "../ui/Disclosure";
import { HereAndNow } from "./HereAndNow";

const REPO = "https://github.com/hungtruongOwolf/deterministic-feed-recovery/blob/main";

interface Props {
  readonly perf: PerfData;
  /** The run the reader just caused, or undefined when WebAssembly is not available. */
  readonly live: LiveRate | undefined;
}

export function Performance({ perf, live }: Props) {
  const costs = assertionCosts(perf);
  const worst = costs.filter((c) => c.significant).sort((a, b) => b.ratio - a.ratio)[0];
  const free = costs.filter((c) => !c.significant).length;
  const hot = perf.shipping.measurements.find((m) => m.name.startsWith("ingest a packet"));
  const batched = perf.handoff.measurements.find((m) => m.name.includes("batches"));

  return (
    <>
      <HereAndNow rate={live} nativePerSecond={hot?.per_second} />

      <div className="perf__headline">
        <Headline
          value={hot === undefined ? "—" : nanos(hot.best_ns)}
          label="to take in one packet"
          note={hot === undefined ? "" : rate(hot.per_second)}
        />
        <Headline
          value={String(perf.shipping.allocations_after_init)}
          label="allocations after start-up"
          note="counted, not asserted"
          good={perf.shipping.allocations_after_init === 0}
        />
        <Headline
          value={batched === undefined ? "—" : nanos(batched.ns_per_message)}
          label="to reach another core"
          note={batched === undefined ? "" : rate(batched.messages_per_second)}
        />
      </div>

      <p className="perf__note">
        Measured natively on a laptop, not in your browser. CPU time in one process — no wire latency is
        claimed, and tick-to-trade needs hardware a cloud VM does not have.
      </p>

      <table className="perf__table">
        <thead>
          <tr>
            <th title="hover a row to see what one operation is">the recovery path</th>
            <th className="perf__num">best</th>
            <th className="perf__num">p99</th>
            <th className="perf__num">rate</th>
          </tr>
        </thead>
        <tbody>
          {perf.shipping.measurements.map((m: Measurement) => (
            <tr key={m.name}>
              {/* The unit is a whole sentence and it matters — "one packet plus one poll, while recovering"
                  is what stops a number being meaningless. But printed in all five rows it was more text than
                  the table. On hover, and in the JSON, where somebody checking a figure will look. */}
              <td title={`one operation is ${m.unit}`}>{m.name}</td>
              <td className="perf__num mono">{nanos(m.best_ns)}</td>
              <td className="perf__num mono">{nanos(m.p99_ns)}</td>
              <td className="perf__num mono">{rate(m.per_second)}</td>
            </tr>
          ))}
        </tbody>
      </table>

      <table className="perf__table">
        <thead>
          <tr>
            <th>across a thread boundary</th>
            <th className="perf__num">per message</th>
            <th className="perf__num">rate</th>
            <th className="perf__num">refused</th>
          </tr>
        </thead>
        <tbody>
          {perf.handoff.measurements.map((h) => (
            <tr key={h.name} className={h.refused > 0 ? "is-refusing" : ""}>
              <td>{h.name}</td>
              <td className="perf__num mono">{nanos(h.ns_per_message)}</td>
              <td className="perf__num mono">{rate(h.messages_per_second)}</td>
              <td className="perf__num mono">{h.refused > 0 ? h.refused.toLocaleString() : "—"}</td>
            </tr>
          ))}
        </tbody>
      </table>

      <Disclosure summary={`what the paranoid assertions cost — ${worst === undefined ? "measured" : `${worst.ratio.toFixed(1)}× at worst, free on the hot path`}`}>
        <p>
          The library can bounds-check every field read. Holding <code>-O3</code> fixed and moving only that
          setting: {worst === undefined ? "" : `${worst.ratio.toFixed(1)}× on the tightest operation, `}
          and no measurable difference on {free} of {costs.length} — including the paths that dominate an
          ingest. <strong>So the checks can stay on in production.</strong>
        </p>
        <ul className="perf__costs">
          {costs.map((c) => (
            <li key={c.name} className={c.significant ? "is-real" : "is-noise"}>
              <span className="mono">{c.significant ? `${c.ratio.toFixed(1)}×` : "—"}</span> {c.name}
            </li>
          ))}
        </ul>
        <p>
          Comparing the <code>dev</code> and <code>bench</code> presets instead gives 55×, and is wrong: one is
          Debug and the other Release, so it prices two variables and reports the sum as one.{" "}
          <a href={`${REPO}/docs/BENCHMARKS.md`}>docs/BENCHMARKS.md</a> has the method, and the three
          measurement bugs found by reading numbers that were too good.
        </p>
      </Disclosure>

      <Disclosure summary="how these were timed, and what the percentiles are over">
        <p>
          Each sample times a batch of operations and divides, because a <code>steady_clock</code> tick is tens
          of nanoseconds and several of these cost less than that. So the percentiles are over{" "}
          <strong>batch means</strong>, not over individual operations: a p99 here cannot show one
          ten-microsecond stall inside a batch of a thousand. It shows that some batches ran consistently
          slower than others, which is what scheduler noise looks like. Figures are minima over{" "}
          {perf.shipping.rounds ?? 1} rounds with the builds run in rotation — benchmark noise only ever adds
          time.
        </p>
      </Disclosure>

      <Disclosure summary="why there is exactly one thread boundary, and what ThreadSanitizer missed">
        <p>
          The recovery core is single-threaded on purpose: determinism is what the project rests on, and a
          multi-threaded core would make the interleaving part of the input. The concurrency sits at one seam,
          where real feed handlers put it — a lock-free ring between the thread that owns the protocol and the
          thread that owns the strategy. A full ring refuses and counts it rather than overwriting, because
          overwriting turns a known backlog into a silent hole.
        </p>
        <p>
          Replacing its release/acquire with relaxed makes it wrong. ThreadSanitizer <strong>passes</strong> the
          broken version; a property test on this arm64 machine fails it 12 times out of 12.{" "}
          <a href={`${REPO}/docs/CONCURRENCY.md`}>docs/CONCURRENCY.md</a>.
        </p>
      </Disclosure>
    </>
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
      <span className="headline__note mono">{note}</span>
    </div>
  );
}

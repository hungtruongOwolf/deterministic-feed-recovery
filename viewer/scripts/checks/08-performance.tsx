// The performance figures: read, never recomputed, and honest about provenance.
//
// Plus the one figure the reader causes rather than the ones I measured: its checks are about provenance
// rather than the number itself, and about the minima rule matching the native tables it sits beside.

import { readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { fastest, ratePerSecond } from "../../src/model/here";
import { assertionCosts, NOISE_FLOOR, parseBenchmarks, parseHandoff } from "../../src/model/perf";
import { Performance } from "../../src/panels/Performance";
import { check } from "./fixtures";

export function run(): void {
  console.log("\nthe performance figures");

  const perf = {
    shipping: parseBenchmarks(readFileSync("public/bench/results.json", "utf8"), "shipping"),
    paranoid: parseBenchmarks(readFileSync("public/bench/results-paranoid.json", "utf8"), "paranoid"),
    handoff: parseHandoff(readFileSync("public/bench/handoff.json", "utf8"), "handoff"),
  };

  check(perf.shipping.assertions === "off", "the headline figures are the shipping configuration");
  check(perf.shipping.allocations_after_init === 0, "the library allocated nothing after start-up");
  check(perf.shipping.measurements.length >= 5, `${perf.shipping.measurements.length} operations measured`);
  check(
    perf.shipping.measurements.every((m) => m.best_ns > 0 && m.p50_ns >= m.best_ns && m.p99_ns >= m.p50_ns),
    "every measurement is ordered best <= p50 <= p99, so none is a placeholder",
  );
  check(
    perf.shipping.measurements.every((m) => m.unit.length > 8 && !/_/.test(m.unit)),
    "every measurement names what one operation is, in words",
  );

  // The hand-off row that refuses has to actually refuse, or it is a row about a condition that did not occur.
  const refusing = perf.handoff.measurements.filter((h) => h.refused > 0);
  check(refusing.length >= 1, `the slow-consumer row really did fill the ring (${refusing[0]?.refused.toLocaleString() ?? 0} refused)`);

  // Batching must beat one-at-a-time, or the reason pop_batch exists is unsupported by the numbers on the page.
  const single = perf.handoff.measurements.find((h) => h.name === "one at a time");
  const batched = perf.handoff.measurements.find((h) => h.name.includes("batches"));
  check(
    single !== undefined && batched !== undefined && batched.ns_per_message < single.ns_per_message,
    "batched draining is measurably cheaper per message than one at a time",
  );

  // The assertion cost must be labelled as noise where it is noise. This is the check that stops the page
  // quoting a 2% difference as a result.
  const costs = assertionCosts(perf);
  check(costs.length === perf.shipping.measurements.length, "every operation is priced against paranoid");
  check(
    costs.every((c) => c.significant === c.ratio >= NOISE_FLOOR),
    "a difference inside the noise floor is marked as noise, not reported as a win",
  );
  check(costs.some((c) => c.significant), "at least one operation is measurably slower with assertions on");

  // A plausible browser run: three acts of 300 messages in a few milliseconds. Values, not zeroes: a panel
  // rendered with an absent measurement is the fallback branch, and that is checked separately below.
  const here = ratePerSecond({ elapsedMs: 6.4, messages: 900, runs: 2 });
  const drawn = renderToStaticMarkup(<Performance perf={perf} live={here} />);
  // The provenance disclaimer is load-bearing: every other figure on the page is computed in the reader's
  // browser and these are not, so the page must not let anybody assume otherwise.
  check(/Measured natively/.test(drawn) && /not in your browser/.test(drawn), "the panel says where these figures came from");
  check(/batch means/.test(drawn), "the panel says what the percentiles are over");
  check(/tick-to-trade/.test(drawn), "the panel names the latency it does not measure");

  // The one figure on this page a reader causes. Its whole value is that it is not committed, so the checks are
  // about provenance rather than about the number: that it is stated as theirs, that the elapsed time it was
  // derived from is shown so the arithmetic is checkable, and that the gap to native is named rather than left
  // for somebody to notice and mistrust.
  check(here !== undefined, "a timed run becomes a rate");
  check(/in your browser/.test(drawn), "the live figure says whose machine produced it");
  check(/6\.4 ms/.test(drawn) && /900 messages/.test(drawn), "and shows the time and the work behind it");
  check(/fastest of 2 runs since you opened this/.test(drawn), "and says which of the runs it is reporting");

  // Minima, like the native tables. Measured first: back-to-back runs of the same acts came out 2.4× apart with
  // the cold one first, so reporting the latest would make the opening figure the worst one.
  const slowFirst = fastest(undefined, { elapsedMs: 12, messages: 900, runs: 1 });
  const thenFast = fastest(slowFirst, { elapsedMs: 4, messages: 900, runs: 1 });
  check(thenFast.elapsedMs === 4 && thenFast.runs === 2, "a faster run replaces a slower one");
  const thenSlow = fastest(thenFast, { elapsedMs: 30, messages: 900, runs: 1 });
  check(thenSlow.elapsedMs === 4 && thenSlow.runs === 3, "and a slower one does not replace it, but is counted");
  // Per message, because the length control changes how much work a run is.
  const longer = fastest(thenFast, { elapsedMs: 8, messages: 2100, runs: 1 });
  check(longer.messages === 2100, "a longer run that is faster per message wins on the per-message figure");
  const gap = drawn.match(/about (\d+)× slower than the tables below/);
  check(gap !== null, "the gap to the native figures is named");
  check(gap !== null && Number(gap[1]) > 1, `native is faster, by ${gap?.[1] ?? "?"}×, which is the honest direction`);
  check(
    drawn.indexOf("in your browser") < drawn.indexOf("Measured natively"),
    "the number a reader made comes before the numbers I made",
  );
  const withoutWasm = renderToStaticMarkup(<Performance perf={perf} live={undefined} />);
  check(
    /did not load/.test(withoutWasm) && !/messages a second/.test(withoutWasm),
    "and without WebAssembly it says so instead of printing a zero",
  );
  check(!/NaN|undefined|Infinity/.test(drawn), "no figure rendered as NaN, undefined or Infinity");
  check((drawn.match(/class="is-real"|class="is-noise"/g) ?? []).length === costs.length, "every assertion cost is drawn");
  check(/allocations after start-up/.test(drawn), "the allocation count is one of the headline figures");
}

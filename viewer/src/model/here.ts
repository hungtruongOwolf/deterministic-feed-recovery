// One number the page did not bring with it.
//
// Every figure in the performance section was measured on a laptop and committed to a JSON file. All of them are
// honest and none of them happened to the reader — which is exactly the complaint that "the numbers look hardcoded",
// and the complaint is correct. A benchmark you cannot cause is a claim.
//
// So this times the runs the page performs anyway. The three acts are recompiled into WebAssembly and executed in
// the visitor's browser on every settings change; timing that call costs one `performance.now()` on each side and
// produces a throughput figure the reader made by pressing something.
//
// What it is not
// --------------
// It is **not** the native ingest number, and reporting it as one would be worse than reporting nothing. This path
// runs through WebAssembly, single-threaded, and serialises every event of every act to JSONL — the trace format is
// the point of the architecture and it is also most of the work being timed here. The figure lands one to two orders
// of magnitude below the native measurement, and that gap is why the native measurement exists rather than an
// embarrassment to be hidden.
//
// The honest claim is narrow: this many messages went through the real state machine, in your browser, just now.

export interface LiveRun {
  /** Wall-clock milliseconds for all three acts, from `performance.now()`. */
  readonly elapsedMs: number;
  /** Messages the venue sent across the three acts — the work the time is over. */
  readonly messages: number;
  /** Runs completed since the page loaded, so a reader can see the number is not fixed. */
  readonly runs: number;
}

/**
 * Keeps the fastest run rather than the latest.
 *
 * Measured before deciding: the same three acts timed back to back came out at 242,778 and then 570,982 messages a
 * second — a 2.4× spread, with the *first* run the slowest, because a cold WebAssembly instance and a JIT that has
 * not seen the loop yet are both being paid for. Showing the latest run would mean the first figure a visitor sees
 * is the worst one and the number lurches every time they touch a control.
 *
 * So: minima, which is what `docs/BENCHMARKS.md` already does for the native tables and for the same reason —
 * noise on a benchmark only ever adds time, so the smallest sample is the one least contaminated by things that
 * are not the code. Using a different method for the figure on the page than for the figures in the repository
 * would undercut both.
 */
export function fastest(previous: LiveRun | undefined, sample: LiveRun): LiveRun {
  if (previous === undefined) {
    return sample;
  }
  const runs = previous.runs + 1;
  const before = previous.elapsedMs / previous.messages;
  const now = sample.elapsedMs / sample.messages;
  // Per message, not per run, because the length control changes how much work a run is.
  return now < before
    ? { elapsedMs: sample.elapsedMs, messages: sample.messages, runs }
    : { elapsedMs: previous.elapsedMs, messages: previous.messages, runs };
}

export interface LiveRate {
  readonly messagesPerSecond: number;
  readonly microsPerMessage: number;
  readonly elapsedMs: number;
  readonly messages: number;
  readonly runs: number;
}

/**
 * Turns a timed run into the two figures worth showing.
 *
 * Returns undefined for a zero-length measurement rather than an infinity. A browser that coarsens
 * `performance.now()` for fingerprinting resistance can return the same value twice, and "∞ messages/second" is
 * the kind of number that discredits every other number beside it.
 */
export function ratePerSecond(run: LiveRun | undefined): LiveRate | undefined {
  if (run === undefined || run.elapsedMs <= 0 || run.messages <= 0) {
    return undefined;
  }
  return {
    messagesPerSecond: (run.messages / run.elapsedMs) * 1000,
    microsPerMessage: (run.elapsedMs * 1000) / run.messages,
    elapsedMs: run.elapsedMs,
    messages: run.messages,
    runs: run.runs,
  };
}

/** "1.2M", "48K", "930" — the same shape the native tables use, so the two are comparable at a glance. */
export function compact(value: number): string {
  if (value >= 1_000_000) {
    return `${(value / 1_000_000).toFixed(1)}M`;
  }
  if (value >= 1_000) {
    return `${Math.round(value / 1_000)}K`;
  }
  return String(Math.round(value));
}

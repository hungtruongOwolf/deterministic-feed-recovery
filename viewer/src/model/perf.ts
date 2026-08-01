// The benchmark JSON's shape, and nothing else.
//
// Same rule as trace.ts and session.ts, and it matters more here than anywhere. Every figure on this page is
// written by the C++ that produced it: no averaging, no unit conversion, no "per second" computed in
// TypeScript from a nanosecond figure. A viewer that did arithmetic on a latency number would be a viewer that
// could get a latency number wrong, and a wrong performance figure is the most embarrassing kind of wrong a
// portfolio can be.
//
// The one thing this file does derive is the *ratio* between two measured figures: the cost of assertions,
// and it is derived rather than stored because it is a comparison between two files, which neither file can
// make on its own. It is a division of two numbers that were both measured; nothing is estimated.

export interface PerfLimit {
  readonly claim: string;
  readonly status: "measured" | "not-measurable" | "simulated";
  readonly note: string;
}

export interface Measurement {
  readonly name: string;
  /** What one operation is, in words. "one packet plus one poll, while recovering". */
  readonly unit: string;
  readonly batch: number;
  readonly samples: number;
  readonly best_ns: number;
  readonly p50_ns: number;
  readonly p99_ns: number;
  readonly worst_ns: number;
  readonly mean_ns: number;
  readonly per_second: number;
}

export interface Benchmarks {
  readonly kind: "benchmarks";
  readonly schema: string;
  readonly assertions: "off" | "fast" | "paranoid";
  readonly allocations_after_init: number;
  readonly rounds?: number;
  readonly measurements: readonly Measurement[];
  readonly limits: readonly PerfLimit[];
}

export interface Handoff {
  readonly name: string;
  readonly note: string;
  readonly ns_per_message: number;
  readonly messages_per_second: number;
  readonly refused: number;
}

export interface HandoffResults {
  readonly kind: "handoff";
  readonly schema: string;
  readonly ring_capacity: number;
  readonly measurements: readonly Handoff[];
  readonly limits: readonly PerfLimit[];
}

export interface Performance {
  readonly shipping: Benchmarks;
  readonly paranoid: Benchmarks;
  readonly handoff: HandoffResults;
}

export class PerfFormatError extends Error {}

function parseObject<T>(text: string, kind: string, what: string): T {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    throw new PerfFormatError(`${what} is not JSON`);
  }
  if (
    typeof parsed !== "object" ||
    parsed === null ||
    (parsed as { kind?: unknown }).kind !== kind
  ) {
    throw new PerfFormatError(`${what} is not a ${kind} document`);
  }
  return parsed as T;
}

export function parseBenchmarks(text: string, what: string): Benchmarks {
  return parseObject<Benchmarks>(text, "benchmarks", what);
}

export function parseHandoff(text: string, what: string): HandoffResults {
  return parseObject<HandoffResults>(text, "handoff", what);
}

/**
 * What paranoid assertions cost on one measurement, as a ratio, or undefined when the two runs disagree about
 * which measurements exist.
 *
 * Below about 1.15 the difference has been seen to *reverse* between rounds, which is the signature of an
 * effect smaller than the machine's noise. So the caller is told the ratio and told whether to believe it,
 * rather than being handed a number that looks like a result. See docs/BENCHMARKS.md.
 */
export interface AssertionCost {
  readonly name: string;
  readonly ratio: number;
  /** False when the difference is inside the noise, and reporting it would be reporting the weather. */
  readonly significant: boolean;
}

export const NOISE_FLOOR = 1.15;

export function assertionCosts(perf: Performance): readonly AssertionCost[] {
  const paranoid = new Map(perf.paranoid.measurements.map((m) => [m.name, m]));
  const costs: AssertionCost[] = [];
  for (const shipping of perf.shipping.measurements) {
    const other = paranoid.get(shipping.name);
    if (other === undefined || shipping.best_ns <= 0) {
      continue;
    }
    const ratio = other.best_ns / shipping.best_ns;
    costs.push({ name: shipping.name, ratio, significant: ratio >= NOISE_FLOOR });
  }
  return costs;
}

/** Nanoseconds, at a precision the figure earns. 0.98 ns and 350 ns want different numbers of digits. */
export function nanos(value: number): string {
  if (value < 10) {
    return `${value.toFixed(2)} ns`;
  }
  if (value < 1000) {
    return `${value.toFixed(1)} ns`;
  }
  return `${(value / 1000).toFixed(2)} µs`;
}

/** A rate a person can read. 195000000 is not a number anybody parses at a glance. */
export function rate(value: number): string {
  if (value >= 1e9) {
    return `${(value / 1e9).toFixed(1)} billion/s`;
  }
  if (value >= 1e6) {
    return `${(value / 1e6).toFixed(0)} million/s`;
  }
  if (value >= 1e3) {
    return `${(value / 1e3).toFixed(0)} thousand/s`;
  }
  return `${value.toFixed(0)}/s`;
}

// Selectors: pure functions from a trace and a scrub position to what should be on screen.
//
// Every one of these reads fields the file already carries. None reconstructs state: see the note
// at the top of trace.ts for why that rule matters more than it looks.

import type { Trace, TraceEvent } from "./trace";

/** The last event at or before `index`, which is what "the run at this moment" means. */
export function eventAt(trace: Trace, index: number): TraceEvent | undefined {
  let found: TraceEvent | undefined;
  for (const event of trace.events) {
    if (event.i > index) {
      break;
    }
    found = event;
  }
  return found;
}

export interface Moment {
  readonly state: string;
  readonly deliveredThrough: number;
  readonly missing: number;
  readonly holes: number;
}

/**
 * The headline numbers at a moment, read from the latest event rather than accumulated.
 *
 * Before the first event there is nothing to read, so the moment is the run's starting state:
 * stated here once rather than guessed at by each panel.
 */
export function momentAt(trace: Trace, index: number): Moment {
  const event = eventAt(trace, index);
  if (event === undefined) {
    return { state: "synchronising", deliveredThrough: 0, missing: 0, holes: 0 };
  }
  return {
    state: event.state,
    deliveredThrough: event.delivered_before,
    missing: event.missing,
    holes: event.holes,
  };
}

/** Events exactly at one packet index, which is what the log pane shows while scrubbing. */
export function eventsAt(trace: Trace, index: number): readonly TraceEvent[] {
  return trace.events.filter((event) => event.i === index);
}

/** Every distinct state the run passed through, with the index at which it began. */
export interface StateSpan {
  readonly state: string;
  readonly from: number;
  readonly to: number;
}

export function stateSpans(trace: Trace): readonly StateSpan[] {
  const spans: StateSpan[] = [];
  for (const event of trace.events) {
    const last = spans.at(-1);
    if (last === undefined || last.state !== event.state) {
      spans.push({ state: event.state, from: event.i, to: event.i });
    } else {
      spans[spans.length - 1] = { ...last, to: event.i };
    }
  }
  const final = spans.at(-1);
  if (final !== undefined) {
    spans[spans.length - 1] = { ...final, to: trace.lastIndex };
  }
  return spans;
}

export interface LineTally {
  readonly line: number;
  /** Packets that carried at least one message no other line had delivered yet. */
  readonly won: number;
  /** Arrived, carried nothing new. On a healthy pair this is most of one line's traffic. */
  readonly duplicate: number;
  /** Failed to decode or to frame, so a real receiver would discard it too. */
  readonly discarded: number;
  readonly faults: number;
}

/**
 * Per-line counts, by tallying event kinds rather than interpreting them.
 *
 * The point of the panel this feeds: losing one line of a redundant pair is invisible in the data,
 * because the stream stays perfect. Unless something counts, the first anyone hears is when the
 * second line fails too.
 */
export function lineTallies(trace: Trace): readonly LineTally[] {
  const tallies = new Map<number, { won: number; duplicate: number; discarded: number; faults: number }>();
  const of = (line: number) => {
    const existing = tallies.get(line);
    if (existing !== undefined) {
      return existing;
    }
    const fresh = { won: 0, duplicate: 0, discarded: 0, faults: 0 };
    tallies.set(line, fresh);
    return fresh;
  };

  for (const event of trace.events) {
    switch (event.event) {
      case "packet_accepted":
      case "gap_opened":
      case "gap_filled":
        of(event.line).won += 1;
        break;
      case "packet_duplicate":
        of(event.line).duplicate += 1;
        break;
      case "packet_discarded":
        of(event.line).discarded += 1;
        break;
      case "fault_applied":
      case "packet_dropped":
      case "packet_duplicated":
      case "packet_delayed":
        of(event.line).faults += 1;
        break;
      default:
        break;
    }
  }

  return [...tallies.entries()]
    .map(([line, counts]) => ({ line, ...counts }))
    .sort((a, b) => a.line - b.line);
}

/** The moment the Glimpse race was decided, if this run reached one. */
export interface SnapshotOutcome {
  readonly rejected: boolean;
  readonly reason: string;
  readonly deliveredThrough: number;
  /** For a rejection, the range that exists in neither the snapshot nor the buffer. */
  readonly unfillableFirst: number;
  readonly unfillableEnd: number;
  readonly at: number;
}

export function snapshotOutcome(trace: Trace): SnapshotOutcome | undefined {
  for (const event of trace.events) {
    if (event.event !== "snapshot_rejected" && event.event !== "snapshot_applied") {
      continue;
    }
    return {
      rejected: event.event === "snapshot_rejected",
      reason: event.reason,
      deliveredThrough: event.delivered_before,
      unfillableFirst: event.first,
      unfillableEnd: event.end,
      at: event.i,
    };
  }
  return undefined;
}

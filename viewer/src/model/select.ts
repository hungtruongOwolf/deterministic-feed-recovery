// Per-line counts: the one selector this file still earns its name for.
//
// Five other functions used to live here (eventAt, momentAt, eventsAt, stateSpans, snapshotOutcome), written in
// the viewer's first commit and never called: App.tsx computes "the moment" inline via useMemo instead, and
// nothing ended up needing the others. Found while adding real unit tests for this file's logic and asking what
// there was to test: dead exports are not a smaller version of that question, they are the same question with
// the answer "nothing", so they are gone rather than covered.

import type { Trace } from "./trace";

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

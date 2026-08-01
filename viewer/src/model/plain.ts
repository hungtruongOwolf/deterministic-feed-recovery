// The run, in words somebody with no idea what a packet is can read.
//
// Measured before this existed: the first screen of the page used nine technical terms before explaining any of them,
// all five section headings were written from the builder's point of view, and **nothing anywhere said why any of it
// mattered**. It described what had been built to somebody who already knew why that was worth building.
//
// Everything here is read from the trace. No new arithmetic: the numbers are the same numbers the drawing uses, said
// differently, because a reader who cannot use the drawing should not be given a second set of figures to reconcile.

import type { Trace, TraceEvent } from "./trace";

export interface PlainOutcome {
  /** "1,240 price updates were sent." */
  readonly sent: string;
  /** "37 of them went missing on the way." */
  readonly lost: string;
  /** What the client did about it, in words. */
  readonly response: string;
  /** The result, and whether it is a good one. */
  readonly result: string;
  readonly good: boolean;
  /** True once the run has finished and the sentences describe an outcome rather than a moment. */
  readonly settled: boolean;
}

/**
 * Says what happened, in the register of a person rather than a protocol.
 *
 * Deliberately says "price update" rather than "message" and "the copy of the price list" rather than "the book".
 * Those are not dumbed down: they are what the things *are*. "Message" and "book" are shorter for somebody who
 * already knows, and meaningless to everybody else.
 */
export function plainly(trace: Trace, event: TraceEvent | undefined): PlainOutcome {
  const sent = trace.header.messages;
  const summary = trace.summary;
  const settled =
    event !== undefined &&
    (event.delivered_before >= sent || event.state === "failed");

  const missing = summary.messages_missing + summary.unfillable_messages;
  const recovered = missing - summary.unfillable_messages;
  const lostForGood = summary.unfillable_messages;

  const asked = summary.retransmit_requests;
  const usedSnapshot = summary.snapshot_requests > 0;

  const response = !settled
    ? "Watch what it does about it."
    : missing === 0
      ? "Nothing went missing on this run, so there was nothing to repair."
      : usedSnapshot
        ? `It asked for them back ${asked} time${asked === 1 ? "" : "s"}, and when that was too late it asked for a fresh copy of the whole price list.`
        : `It noticed, and asked the exchange to send them again, ${asked} time${asked === 1 ? "" : "s"}.`;

  const result = !settled
    ? "The run is still going."
    : lostForGood === 0
      ? "Every one came back. The price list it ended up with is identical to the one that never lost anything."
      : `${lostForGood.toLocaleString()} never came back. So the price list is wrong, and the important part is that it *knows*, and refuses to hand it on rather than pretending.`;

  return {
    sent: `${sent.toLocaleString()} price updates were sent.`,
    lost:
      missing === 0
        ? "None of them went missing."
        : `${missing.toLocaleString()} went missing on the way, ${recovered.toLocaleString()} temporarily, ${lostForGood.toLocaleString()} for good.`,
    response,
    result,
    good: settled && lostForGood === 0,
    settled,
  };
}

// Whether the book this act rebuilt is the book that would have existed.
//
// The one thing the page most needs to say and could not. A visitor watching a bid and an ask tick has no way to
// know they are looking at the project's central claim — that the book after loss and repair equals the book that
// lost nothing. The numbers were on the page; the *argument* was not.
//
// Both sides come from the trace: the run's own book on each event, the loss-free book in the summary. Nothing here
// applies a price level or computes a book, which is the format's one rule.

import type { Trace, TraceEvent } from "./trace";

export interface BookVerdict {
  /** True when the rebuilt book equals the book that lost nothing. */
  readonly matches: boolean;
  readonly traded: number;
  readonly referenceTraded: number;
  /** Shares the run never saw. Zero when it matches. */
  readonly missingShares: number;
  /** False before any message has been delivered — there is nothing to compare yet. */
  readonly comparable: boolean;
}

/**
 * Compares the book at this moment against the loss-free book for the whole run.
 *
 * Mid-run the two legitimately differ: the reference is the *final* book and the run has not finished. So this is
 * only a verdict once the run has caught up, and `comparable` says when. Reporting "diverged" halfway through would
 * be reporting that a film is not over.
 */
export function verdictAt(trace: Trace, event: TraceEvent | undefined): BookVerdict {
  const referenceTraded = trace.summary.reference_traded;
  if (event === undefined) {
    return { matches: false, traded: 0, referenceTraded, missingShares: 0, comparable: false };
  }
  // Comparable once the run is over, which is two conditions rather than one.
  //
  // The first version used only "delivery reached the end of the feed", and act III never says that: the client
  // *fails*, so delivery stalls short and the page called a finished run "still playing". A client that has failed
  // is not going to deliver anything else, and that is exactly the act whose book is worth a verdict.
  const finished =
    event.delivered_through >= trace.header.messages || event.state === "failed";
  const matches =
    event.best_bid === trace.summary.reference_bid &&
    event.best_ask === trace.summary.reference_ask &&
    event.traded_shares === referenceTraded;
  return {
    matches,
    traded: event.traded_shares,
    referenceTraded,
    missingShares: referenceTraded > event.traded_shares ? referenceTraded - event.traded_shares : 0,
    comparable: finished,
  };
}

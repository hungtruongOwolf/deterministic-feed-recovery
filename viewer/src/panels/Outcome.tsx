// What just happened, in four sentences and no jargon.
//
// The film is legible to somebody who knows what a sequence number is. This is the same run for everybody else, and it
// is not a simplification: "the price list came out identical to the one that never lost anything" *is* the claim.
// "The book after loss and repair equals the reference book" is the same sentence for people who already agree it
// matters.

import type { PlainOutcome } from "../model/plain";

interface Props {
  readonly outcome: PlainOutcome;
}

export function Outcome({ outcome }: Props) {
  return (
    <div className={`outcome ${outcome.settled ? (outcome.good ? "is-good" : "is-bad") : "is-running"}`}>
      <p className="outcome__line">{outcome.sent}</p>
      <p className="outcome__line">{outcome.lost}</p>
      <p className="outcome__line">{outcome.response}</p>
      <p className="outcome__line outcome__line--result">{outcome.result}</p>
    </div>
  );
}

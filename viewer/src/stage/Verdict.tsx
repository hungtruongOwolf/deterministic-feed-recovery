// The claim, stated on the drawing while the run makes it.
//
// The page carried a bid and an ask, and a visitor had no way to know they were looking at the project's central
// argument. Numbers were present; the argument was not. This is the sentence that turns one into the other:
//
//   **is this the book that would have existed if nothing had been lost?**
//
// It stays quiet while the run is still playing, because a difference halfway through is a film that is not over
// rather than a defect. Once delivery reaches the end of the feed, it answers.

import type { BookVerdict } from "../model/book";

interface Props {
  readonly verdict: BookVerdict;
  readonly x: number;
  readonly y: number;
}

export function Verdict({ verdict, x, y }: Props) {
  if (!verdict.comparable) {
    return (
      <g className="verdict verdict--waiting" transform={`translate(${x},${y})`}>
        <text className="verdict__line" x={0} y={0}>
          still playing — the book is compared when the feed ends
        </text>
      </g>
    );
  }

  return (
    <g
      className={`verdict ${verdict.matches ? "verdict--holds" : "verdict--broken"}`}
      transform={`translate(${x},${y})`}
    >
      <text className="verdict__head" x={0} y={0}>
        {verdict.matches ? "THE BOOK IS RIGHT" : "THE BOOK IS INCOMPLETE"}
      </text>
      <text className="verdict__line" x={0} y={16}>
        {verdict.matches
          ? "identical to the book that lost nothing"
          : `${verdict.missingShares.toLocaleString()} of ${verdict.referenceTraded.toLocaleString()} shares never arrived`}
      </text>
      <text className="verdict__line verdict__line--quiet" x={0} y={30}>
        {verdict.matches
          ? "every packet lost was found again"
          : "and the client says so rather than publishing it"}
      </text>
    </g>
  );
}

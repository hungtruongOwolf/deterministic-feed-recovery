// The top of the order book, drawn beside the run.
//
// This is what the whole message layer was for. Everything else on the sheet is *bookkeeping* — which sequence
// numbers arrived, which holes are open — and a trading system does not care about sequence numbers. It cares
// whether the book is right. Until the trace carried one, the strongest claim the project makes lived only in a
// test file and a visitor could not see it.
//
// Top of book and a level count, not full depth. A full ladder at this size is six numbers nobody can read while
// something is moving, and the trace deliberately carries only the top for the same reason. The level count is
// there so the drawing says "3 levels" rather than implying the book is one deep.
//
// Nothing here computes anything. Every number is a field on the event.

import type { TraceEvent } from "../model/trace";

const SCALE = 10_000;

interface Props {
  readonly event: TraceEvent;
  readonly x: number;
  readonly y: number;
}

function money(raw: number): string {
  return raw === 0 ? "—" : (raw / SCALE).toFixed(4);
}

export function Quote({ event, x, y }: Props) {
  const crossed =
    event.best_bid !== 0 && event.best_ask !== 0 && event.best_bid >= event.best_ask;

  return (
    <g className="quote" transform={`translate(${x},${y})`}>
      <text className="quote__head" x={0} y={0}>
        THE BOOK
      </text>

      {/* The ask above the bid, the way a book is always written. */}
      <text className="quote__label" x={0} y={19}>
        ask
      </text>
      <text className={`quote__price quote__price--ask ${crossed ? "is-crossed" : ""}`} x={30} y={19}>
        {money(event.best_ask)}
      </text>
      <text className="quote__size" x={104} y={19}>
        {event.best_ask_size > 0 ? `×${event.best_ask_size}` : ""}
      </text>
      <text className="quote__depth" x={160} y={19}>
        {event.ask_levels > 0 ? `${event.ask_levels} lvl` : ""}
      </text>

      <text className="quote__label" x={0} y={37}>
        bid
      </text>
      <text className={`quote__price quote__price--bid ${crossed ? "is-crossed" : ""}`} x={30} y={37}>
        {money(event.best_bid)}
      </text>
      <text className="quote__size" x={104} y={37}>
        {event.best_bid_size > 0 ? `×${event.best_bid_size}` : ""}
      </text>
      <text className="quote__depth" x={160} y={37}>
        {event.bid_levels > 0 ? `${event.bid_levels} lvl` : ""}
      </text>

      {/* Volume, cumulative, so it grows rather than needing two events differenced. */}
      <text className="quote__label" x={0} y={57}>
        traded
      </text>
      <text className="quote__size" x={44} y={57}>
        {event.traded_shares.toLocaleString()} shares
      </text>
    </g>
  );
}

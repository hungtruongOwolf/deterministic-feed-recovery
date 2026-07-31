// What the client knows, one cell per message.
//
// The second of the drawing's two spaces, and the one that carries the stakes. A count of six missing
// messages is a fact you have to trust; six blank cells punched out of a solid block is a thing you see. It
// also makes the difference between "not yet" and "never" visible, which is the distinction the whole
// project turns on — an empty cell ahead of the frontier is ordinary, an empty cell behind it is a hole.
//
// Every cell's state is read from the event: filled below `delivered_through` unless it falls inside one of
// the `gaps` the trace carries. Nothing is accumulated here.

import type { TraceEvent } from "../model/trace";
import { BOOK, gridFor } from "./layout";

interface Props {
  readonly event: TraceEvent | undefined;
  readonly messages: number;
}

export function BookGrid({ event, messages }: Props) {
  const grid = gridFor(messages);
  const delivered = event?.delivered_through ?? 0;
  const gaps = event?.gaps ?? [];
  const undrawn = Math.max(0, (event?.holes ?? 0) - gaps.length);

  const missing = (sequence: number) => gaps.some(([first, end]) => sequence >= first && sequence < end);

  const cells = [];
  for (let i = 0; i < messages; i += 1) {
    const sequence = i + 1;
    const col = i % grid.cols;
    const row = Math.floor(i / grid.cols);
    const state = missing(sequence) ? "hole" : sequence < delivered ? "have" : "ahead";
    cells.push(
      <rect
        key={i}
        className={`book__cell book__cell--${state}`}
        x={grid.x + col * grid.cell}
        y={grid.y + row * grid.cell}
        width={grid.cell - 1.4}
        height={grid.cell - 1.4}
      />,
    );
  }

  // The frontier: everything to its left has been decided, everything to its right has not happened yet.
  const frontierIndex = Math.max(0, Math.min(messages, delivered) - 1);
  const fCol = frontierIndex % grid.cols;
  const fRow = Math.floor(frontierIndex / grid.cols);

  return (
    <g className="book">
      <rect className="plan__region" x={BOOK.x} y={BOOK.y} width={BOOK.w} height={BOOK.h} />
      <text className="region__title" x={BOOK.x + 12} y={BOOK.y + 18}>
        WHAT THE CLIENT KNOWS
      </text>
      <text className="region__sub" x={BOOK.x + 12} y={BOOK.y + 32}>
        one cell per message, {messages} in this run
      </text>

      {cells}

      {delivered > 0 && (
        <rect
          className="book__frontier"
          x={grid.x + fCol * grid.cell - 1}
          y={grid.y + fRow * grid.cell - 1}
          width={grid.cell + 0.4}
          height={grid.cell + 0.4}
        />
      )}

      <g className="book__legend">
        {[
          { cls: "have", label: "delivered" },
          { cls: "hole", label: "missing — a hole" },
          { cls: "ahead", label: "not sent yet" },
        ].map((entry, i) => (
          <g key={entry.cls} transform={`translate(${BOOK.x + 14 + i * 124}, ${BOOK.y + BOOK.h - 40})`}>
            <rect className={`book__cell book__cell--${entry.cls}`} x={0} y={0} width={9} height={9} />
            <text x={14} y={8}>
              {entry.label}
            </text>
          </g>
        ))}
      </g>

      {undrawn > 0 && (
        <text className="book__more" x={BOOK.x + 14} y={BOOK.y + BOOK.h - 18}>
          and {undrawn} more holes than the grid draws
        </text>
      )}
    </g>
  );
}

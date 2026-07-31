// The message axis: what has been delivered, and what is missing.
//
// The one place in the viewer where a hole is a *shape* rather than a number. A count of six missing messages
// is a fact; a red band sitting inside a solid bar is a thing a reader understands without being told.
//
// Both are read from the event — `delivered_through` and `gaps` — never accumulated here.

import type { TraceEvent } from "../model/trace";
import { SEQUENCE, lerp } from "./geometry";

interface Props {
  readonly event: TraceEvent | undefined;
  readonly highest: number;
}

export function SequenceBand({ event, highest }: Props) {
  const span = Math.max(1, highest);
  const delivered = event?.delivered_through ?? 0;
  const gaps = event?.gaps ?? [];
  const undrawn = Math.max(0, (event?.holes ?? 0) - gaps.length);

  const at = (sequence: number) =>
    lerp(SEQUENCE.left, SEQUENCE.right, Math.max(0, Math.min(1, sequence / span)));

  return (
    <g className="band">
      <text className="band__label" x={SEQUENCE.left} y={SEQUENCE.y - 10}>
        MESSAGES
      </text>
      <text className="band__scale" x={SEQUENCE.right} y={SEQUENCE.y - 10} textAnchor="end">
        1 … {span}
      </text>

      {/* The whole stream, as a hairline outline: what the venue sent. */}
      <rect
        className="band__outline"
        x={SEQUENCE.left}
        y={SEQUENCE.y}
        width={SEQUENCE.right - SEQUENCE.left}
        height={SEQUENCE.height}
      />

      {/* Delivered, solid. */}
      <rect
        className="band__delivered"
        x={SEQUENCE.left}
        y={SEQUENCE.y}
        width={Math.max(0, at(delivered) - SEQUENCE.left)}
        height={SEQUENCE.height}
      />

      {/* Holes, cut out of it. A hole narrower than a pixel is still drawn at one pixel: a message that is
          missing must be visible, or the picture flatters the client. */}
      {gaps.map(([first, end]) => (
        <g key={`${first}-${end}`}>
          <rect
            className="band__hole"
            x={at(first)}
            y={SEQUENCE.y}
            width={Math.max(1.5, at(end) - at(first))}
            height={SEQUENCE.height}
          />
          <text className="band__hole-label" x={at(first)} y={SEQUENCE.y + SEQUENCE.height + 13}>
            {first}–{end - 1}
          </text>
        </g>
      ))}

      {/* The watermark: the frontier of what has crossed. */}
      <line
        className="band__mark"
        x1={at(delivered)}
        y1={SEQUENCE.y - 5}
        x2={at(delivered)}
        y2={SEQUENCE.y + SEQUENCE.height + 5}
      />
      <text className="band__mark-label" x={at(delivered) + 5} y={SEQUENCE.y - 10}>
        {delivered}
      </text>

      {undrawn > 0 && (
        <text className="band__more" x={SEQUENCE.right} y={SEQUENCE.y + SEQUENCE.height + 13} textAnchor="end">
          and {undrawn} more missing, not drawn
        </text>
      )}
    </g>
  );
}

// The three defences, and which of them this run has.
//
// The thing the first two viewers were missing entirely. The three bundled runs are not three demos; they
// are the *same system with layers removed*, one at a time, so that the layer underneath can be watched
// doing its job. A run with redundancy working never exercises retransmission, because there is nothing
// left for it to do — so to see retransmission you take the second line away on purpose.
//
// Drawn as a ladder with the removed rungs struck out, the experiment explains itself.

import { LADDER } from "./layout";

export interface Rung {
  readonly ordinal: string;
  readonly name: string;
  readonly cost: string;
  readonly present: boolean;
  readonly exercised: boolean;
}

export function rungsFor(lines: number, servedRetransmits: number, snapshotAsked: number): readonly Rung[] {
  return [
    {
      ordinal: "1",
      name: "TWO LINES",
      cost: "costs bandwidth, costs no time",
      present: lines > 1,
      exercised: lines > 1,
    },
    {
      ordinal: "2",
      name: "ASK FOR IT BACK",
      cost: "costs a round trip, and expires",
      present: true,
      exercised: servedRetransmits > 0,
    },
    {
      ordinal: "3",
      name: "REBUILD FROM A SNAPSHOT",
      cost: "costs seconds, and blind while it runs",
      present: true,
      exercised: snapshotAsked > 0,
    },
  ];
}

export function Ladder({ rungs }: { readonly rungs: readonly Rung[] }) {
  const width = (LADDER.w - 16) / rungs.length;
  return (
    <g className="ladder">
      <text className="region__title" x={LADDER.x} y={LADDER.y - 4}>
        THE THREE DEFENCES AGAINST A LOST PACKET — CHEAPEST FIRST
      </text>
      {rungs.map((rung, i) => (
        <g
          key={rung.name}
          className={`ladder__rung ${rung.present ? "" : "is-absent"} ${rung.exercised ? "is-on" : ""}`}
          transform={`translate(${LADDER.x + i * (width + 8)}, ${LADDER.y + 6})`}
        >
          <rect x={0} y={0} width={width} height={LADDER.h - 6} />
          <text className="ladder__ordinal" x={11} y={25}>
            {rung.ordinal}
          </text>
          <text className="ladder__name" x={30} y={18}>
            {rung.name}
          </text>
          <text className="ladder__cost" x={30} y={32}>
            {rung.present ? rung.cost : "removed in this run, on purpose"}
          </text>
          {!rung.present && (
            <g className="plan__strike">
              <line x1={4} y1={4} x2={width - 4} y2={LADDER.h - 10} />
              <line x1={width - 4} y1={4} x2={4} y2={LADDER.h - 10} />
            </g>
          )}
        </g>
      ))}
    </g>
  );
}

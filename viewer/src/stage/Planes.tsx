// The three defences, drawn as the three stacked planes they are.
//
// This replaces a legend that listed them. A list says "there are three"; a stack says how they relate — each
// one sits *under* the one that failed, and a run escalates by falling to the next plane down. The column at
// the receiver's edge is that fall, and the marker on it says which layer the run is at right now.
//
// A defence a run does not have is drawn as an empty outline with its parts struck out, so the experiment is
// visible in the drawing rather than in a dropdown label.

import {
  ENGINE,
  ESCALATION,
  LAYER_DROP,
  PLANE,
  RECEIVER,
  RETX,
  ROUTES,
  SNAP,
  SWITCHES,
  planeOutline,
  polyline,
  project,
} from "./layout";

export interface Rung {
  readonly ordinal: string;
  readonly name: string;
  readonly cost: string;
  readonly present: boolean;
  readonly reached: boolean;
}

export function rungsFor(lines: number, servedRetransmits: number, snapshotAsked: number): readonly Rung[] {
  return [
    {
      ordinal: "1",
      name: "TWO LINES",
      cost: "costs bandwidth all day · costs no time at all",
      present: lines > 1,
      reached: true,
    },
    {
      ordinal: "2",
      name: "ASK FOR IT BACK",
      cost: "costs a round trip · and it expires",
      present: true,
      reached: servedRetransmits > 0 || snapshotAsked > 0,
    },
    {
      ordinal: "3",
      name: "REBUILD FROM A SNAPSHOT",
      cost: "costs seconds · blind while it runs",
      present: true,
      reached: snapshotAsked > 0,
    },
  ];
}

function Node({
  box,
  title,
  sub,
  active,
  struck,
}: {
  readonly box: { x: number; y: number; w: number; h: number };
  readonly title: string;
  readonly sub?: string;
  readonly active?: boolean;
  readonly struck?: boolean;
}) {
  return (
    <g className={`plan__node ${active === true ? "is-active" : ""} ${struck === true ? "is-struck" : ""}`}>
      <rect x={box.x} y={box.y} width={box.w} height={box.h} />
      <text
        className="plan__node-title"
        x={box.x + box.w / 2}
        y={box.y + (sub === undefined ? box.h / 2 + 3.5 : box.h / 2 - 2)}
        textAnchor="middle"
      >
        {title}
      </text>
      {sub !== undefined && (
        <text className="plan__node-sub" x={box.x + box.w / 2} y={box.y + box.h / 2 + 11} textAnchor="middle">
          {sub}
        </text>
      )}
      {struck === true && (
        <g className="plan__strike">
          <line x1={box.x + 3} y1={box.y + 3} x2={box.x + box.w - 3} y2={box.y + box.h - 3} />
          <line x1={box.x + box.w - 3} y1={box.y + 3} x2={box.x + 3} y2={box.y + box.h - 3} />
        </g>
      )}
    </g>
  );
}

interface Props {
  readonly rungs: readonly Rung[];
  readonly retransmitAvailable: boolean;
  /** Which plane the run has fallen to, 0–2. */
  readonly atLayer: number;
  readonly active: "engine" | "receiver" | "retx" | "snap" | undefined;
}

export function Planes({ rungs, retransmitAvailable, atLayer, active }: Props) {
  const bothLines = rungs[0]?.present === true;
  const label = (layer: number) => project({ x: 0, y: PLANE.d, layer });

  return (
    <g className="planes">
      {/* Drawn back to front so a nearer plane overlaps the one behind it. */}
      {[2, 1, 0].map((layer) => {
        const rung = rungs[layer] as Rung;
        const at = label(layer);
        return (
          <g
            key={layer}
            className={`plane ${rung.present ? "" : "is-absent"} ${rung.reached ? "is-reached" : ""} ${
              atLayer === layer ? "is-here" : ""
            }`}
          >
            <path className="plane__face" d={planeOutline(layer)} />
            <text className="plane__ordinal" x={at.x - 26} y={at.y - 34}>
              {rung.ordinal}
            </text>
            <text className="plane__name" x={at.x + 4} y={at.y - 44}>
              {rung.name}
            </text>
            <text className="plane__cost" x={at.x + 4} y={at.y - 32}>
              {rung.present ? rung.cost : "REMOVED IN THIS RUN, ON PURPOSE"}
            </text>
          </g>
        );
      })}

      {/* The column the escalation falls down. */}
      <path
        className="plan__column"
        d={`M ${ESCALATION[0]!.x} ${ESCALATION[0]!.y} L ${ESCALATION[2]!.x} ${ESCALATION[2]!.y}`}
      />
      {ESCALATION.map((p, layer) => (
        <circle
          key={layer}
          className={`plan__stop ${layer === atLayer ? "is-here" : ""} ${layer < atLayer ? "is-past" : ""}`}
          cx={p.x}
          cy={p.y}
          r={layer === atLayer ? 6 : 3.5}
        />
      ))}
      <text className="plan__column-label" x={ESCALATION[2]!.x + 12} y={ESCALATION[2]!.y + LAYER_DROP / 3}>
        ↓ ESCALATES WHEN THE LAYER ABOVE CANNOT HELP
      </text>

      {/* Plane 0: the two multicast paths. */}
      <path className="plan__route" d={polyline(ROUTES.lineA)} />
      <path className={`plan__route ${bothLines ? "" : "is-absent"}`} d={polyline(ROUTES.lineB)} />
      {SWITCHES.a.map((s) => (
        <Node key={s.name} box={s} title={s.name} />
      ))}
      {SWITCHES.b.map((s) => (
        <Node key={s.name} box={s} title={s.name} struck={!bothLines} />
      ))}
      <Node box={ENGINE} title="MATCHING" sub="ENGINE" active={active === "engine"} />
      <Node box={RECEIVER} title="RECEIVER" sub={bothLines ? "two NICs" : "one NIC"} active={active === "receiver"} />

      {/* Planes 1 and 2: the TCP services. */}
      <path className="plan__tcp" d={polyline(ROUTES.retx)} />
      <path className="plan__tcp" d={polyline(ROUTES.snap)} />
      <Node
        box={RETX}
        title="RETRANSMIT"
        sub={retransmitAvailable ? "short window" : "too late in this run"}
        active={active === "retx"}
        struck={!retransmitAvailable}
      />
      <Node box={SNAP} title="SNAPSHOT" sub="rebuilds the book" active={active === "snap"} />
    </g>
  );
}

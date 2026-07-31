// The path a packet takes, drawn as a plan.
//
// Two multicast networks bow apart and rejoin at the receiver, because that is physically what redundancy
// *is*: separate switches, separate fibre, so that one failing does not take the other with it. Drawn this
// way it needs no caption — the same packet is visibly on both arcs, and losing one arc costs nothing.
//
// Below, on their own, the two TCP services. They are a different network with different properties: slower,
// reliable, and able to say no. Keeping them off the multicast plan is the point rather than tidiness.

import {
  ENGINE,
  LINE_A_Y,
  LINE_B_Y,
  PLAN,
  RECEIVER,
  RETX,
  ROUTES,
  SNAP,
  SWITCHES,
  path,
} from "./layout";

interface Props {
  /** How many multicast paths this run has. One means the second is drawn struck out. */
  readonly lines: number;
  /** False when the retransmit service is unreachable in this run, so it is drawn struck out. */
  readonly retransmitAvailable: boolean;
  readonly snapshotUsed: boolean;
  /** Which node the current step is happening at, for a soft highlight. */
  readonly active: "engine" | "receiver" | "retx" | "snap" | undefined;
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
      <text className="plan__node-title" x={box.x + box.w / 2} y={box.y + (sub === undefined ? box.h / 2 + 4 : box.h / 2 - 2)} textAnchor="middle">
        {title}
      </text>
      {sub !== undefined && (
        <text className="plan__node-sub" x={box.x + box.w / 2} y={box.y + box.h / 2 + 12} textAnchor="middle">
          {sub}
        </text>
      )}
      {struck === true && (
        <g className="plan__strike">
          <line x1={box.x + 4} y1={box.y + 4} x2={box.x + box.w - 4} y2={box.y + box.h - 4} />
          <line x1={box.x + box.w - 4} y1={box.y + 4} x2={box.x + 4} y2={box.y + box.h - 4} />
        </g>
      )}
    </g>
  );
}

export function Plan({ lines, retransmitAvailable, snapshotUsed, active }: Props) {
  const bothLines = lines > 1;

  return (
    <g className="plan">
      <rect className="plan__region" x={PLAN.x} y={PLAN.y} width={PLAN.w} height={PLAN.h} />
      <text className="region__title" x={PLAN.x + 12} y={PLAN.y + 18}>
        THE PATH — WHERE A PACKET TRAVELS
      </text>

      {/* The two multicast networks. */}
      <path className="plan__route plan__route--a" d={path(ROUTES.lineA)} />
      <path className={`plan__route plan__route--b ${bothLines ? "" : "is-absent"}`} d={path(ROUTES.lineB)} />

      <text className="plan__route-label" x={PLAN.x + 150} y={LINE_A_Y - 14}>
        LINE A · UDP MULTICAST
      </text>
      <text className={`plan__route-label ${bothLines ? "" : "is-absent"}`} x={PLAN.x + 150} y={LINE_B_Y + 22}>
        LINE B · UDP MULTICAST{bothLines ? "" : " — NOT SUBSCRIBED IN THIS RUN"}
      </text>

      {SWITCHES.a.map((s) => (
        <Node key={s.name} box={s} title={s.name} />
      ))}
      {SWITCHES.b.map((s) => (
        <Node key={s.name} box={s} title={s.name} struck={!bothLines} />
      ))}

      {/* The TCP services, on their own network. */}
      <path className="plan__tcp" d={path(ROUTES.retx)} />
      <path className="plan__tcp" d={path(ROUTES.snap)} />
      <text className="plan__route-label" x={RETX.x} y={RETX.y - 10}>
        TCP · ASKING FOR THINGS BACK
      </text>

      <Node
        box={ENGINE}
        title="MATCHING"
        sub="ENGINE"
        active={active === "engine"}
      />
      <Node
        box={RETX}
        title="RETRANSMIT"
        sub={retransmitAvailable ? "keeps a short window" : "unreachable in this run"}
        active={active === "retx"}
        struck={!retransmitAvailable}
      />
      <Node
        box={SNAP}
        title="SNAPSHOT"
        sub={snapshotUsed ? "rebuilds the book" : "not needed here"}
        active={active === "snap"}
      />
      <Node
        box={RECEIVER}
        title="RECEIVER"
        sub={bothLines ? "two NICs" : "one NIC"}
        active={active === "receiver"}
      />
    </g>
  );
}

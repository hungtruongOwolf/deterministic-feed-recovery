// The scene: the ladder above, the path on the left, the book on the right, and a packet moving between them.
//
// The composition carries the argument. Damage happens in the plan; a few beats later a cell in the grid is
// still blank. Watching the second follow the first is what makes "lost data you do not know about" a thing
// somebody can see rather than a sentence they have to accept.

import type { Beat } from "../model/story";
import type { Trace } from "../model/trace";
import { BookGrid } from "./BookGrid";
import { Ladder, rungsFor } from "./Ladder";
import { Plan } from "./Plan";
import { ROUTES, along, ease, type Point, type RouteName } from "./layout";

interface Props {
  readonly trace: Trace;
  readonly beat: Beat | undefined;
  readonly progress: number;
  readonly trail: readonly Beat[];
  readonly messages: number;
}

/** Which drawn route a step travels on, and in which direction. */
function routeOf(beat: Beat, lines: number): { readonly route: RouteName; readonly reversed: boolean } {
  if (beat.lane === "request") {
    return { route: "retx", reversed: beat.direction === "out" };
  }
  if (beat.lane === "snapshot") {
    return { route: "snap", reversed: beat.direction === "out" };
  }
  // Alternate the two multicast paths so a redundant run visibly uses both.
  const onB = lines > 1 && beat.event.line === 1;
  return { route: onB ? "lineB" : "lineA", reversed: false };
}

function Packet({ beat, progress, lines }: { readonly beat: Beat; readonly progress: number; readonly lines: number }) {
  const { route, reversed } = routeOf(beat, lines);
  const t = ease(progress);
  const stops = beat.fate === "vanish" ? 0.55 : beat.fate === "reject" ? 0.92 : 1;
  const travelled = beat.fate === "lag" ? t * 0.7 : Math.min(t, stops);
  const at: Point = along(ROUTES[route], travelled, reversed);

  const dying = beat.fate === "vanish" && t > stops;
  const opacity = dying ? Math.max(0, 1 - (t - stops) / 0.28) : 1;
  const wide = beat.lane === "snapshot";

  return (
    <g className={`packet packet--${beat.tone}`} opacity={opacity} transform={`translate(${at.x}, ${at.y})`}>
      {wide ? (
        <rect className="packet__body packet__body--wide" x={-22} y={-8} width={44} height={16} />
      ) : beat.lane === "request" ? (
        <path className="packet__body" d="M -9 0 L 0 -7 L 9 0 L 0 7 Z" />
      ) : (
        <rect className="packet__body" x={-9} y={-6} width={18} height={12} />
      )}
      {beat.fate === "twin" && <rect className="packet__body packet__body--twin" x={-3} y={-10} width={18} height={12} />}
      {dying && (
        <g className="packet__burst">
          <line x1={-8} y1={-8} x2={8} y2={8} />
          <line x1={8} y1={-8} x2={-8} y2={8} />
        </g>
      )}
      {beat.label !== "" && (
        <text className="packet__label" x={0} y={-13} textAnchor="middle">
          {beat.label}
        </text>
      )}
    </g>
  );
}

function activeNode(beat: Beat | undefined): "engine" | "receiver" | "retx" | "snap" | undefined {
  if (beat === undefined) {
    return undefined;
  }
  if (beat.lane === "snapshot") {
    return beat.direction === "out" ? "snap" : "receiver";
  }
  if (beat.lane === "request") {
    return beat.direction === "out" ? "retx" : "receiver";
  }
  return beat.direction === "out" ? "engine" : "receiver";
}

export function Stage({ trace, beat, progress, trail, messages }: Props) {
  const lines = trace.header.lines;
  const rungs = rungsFor(lines, trace.summary.retransmits_served, trace.summary.snapshot_requests);
  const retransmitAvailable = trace.summary.retransmits_served > 0 || trace.summary.retransmit_requests === 0;

  return (
    <g>
      <Ladder rungs={rungs} />
      <Plan
        lines={lines}
        retransmitAvailable={retransmitAvailable}
        snapshotUsed={trace.summary.snapshot_requests > 0}
        active={activeNode(beat)}
      />
      <BookGrid event={beat?.event} messages={messages} />

      {trail.map((past, i) => (
        <g key={past.at} opacity={0.08 + 0.1 * (trail.length - i)}>
          <Packet beat={past} progress={1} lines={lines} />
        </g>
      ))}
      {beat !== undefined && <Packet beat={beat} progress={progress} lines={lines} />}
    </g>
  );
}

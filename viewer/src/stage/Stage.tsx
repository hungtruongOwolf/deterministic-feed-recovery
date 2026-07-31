// The scene: three stacked planes on the left, the book on the right, a packet moving through them.
//
// The composition is the argument. A packet is damaged on plane 0; a few beats later a cell on the right is
// still blank; and when plane 0 cannot fix it the run falls to plane 1, then to plane 2. Escalation is
// downward movement, which is what makes the three defences read as one system rather than three options.

import type { Beat } from "../model/story";
import type { Trace } from "../model/trace";
import { BookGrid } from "./BookGrid";
import { Planes, rungsFor } from "./Planes";
import { ROUTES, along, ease, type Point, type RouteName } from "./layout";

interface Props {
  readonly trace: Trace;
  readonly beat: Beat | undefined;
  readonly progress: number;
  readonly trail: readonly Beat[];
  readonly messages: number;
}

function routeOf(beat: Beat, lines: number): { readonly route: RouteName; readonly reversed: boolean } {
  if (beat.lane === "request") {
    return { route: "retx", reversed: beat.direction === "out" };
  }
  if (beat.lane === "snapshot") {
    return { route: "snap", reversed: beat.direction === "out" };
  }
  const onB = lines > 1 && beat.event.line === 1;
  return { route: onB ? "lineB" : "lineA", reversed: false };
}

/** Which plane the run has fallen to, read from the step rather than inferred. */
function layerOf(beat: Beat | undefined): number {
  if (beat === undefined) {
    return 0;
  }
  if (beat.lane === "snapshot" || beat.event.state === "failed" || beat.event.state === "replaying") {
    return 2;
  }
  if (beat.lane === "request" || beat.event.state === "recovering") {
    return 1;
  }
  return 0;
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
        <rect className="packet__body packet__body--wide" x={-20} y={-7} width={40} height={14} />
      ) : beat.lane === "request" ? (
        <path className="packet__body" d="M -8 0 L 0 -6 L 8 0 L 0 6 Z" />
      ) : (
        <rect className="packet__body" x={-8} y={-5.5} width={16} height={11} />
      )}
      {beat.fate === "twin" && <rect className="packet__body packet__body--twin" x={-3} y={-9} width={16} height={11} />}
      {dying && (
        <g className="packet__burst">
          <line x1={-7} y1={-7} x2={7} y2={7} />
          <line x1={7} y1={-7} x2={-7} y2={7} />
        </g>
      )}
      {beat.label !== "" && (
        <text className="packet__label" x={0} y={-12} textAnchor="middle">
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
      <Planes rungs={rungs} retransmitAvailable={retransmitAvailable} atLayer={layerOf(beat)} active={activeNode(beat)} />
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

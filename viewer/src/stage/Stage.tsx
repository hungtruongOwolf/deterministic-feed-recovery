// The scene: a venue on the left, a client on the right, and packets crossing the space between them.
//
// Everything here is drawn from the current beat and its fraction. The venue and the client are plan symbols
// rather than boxes with labels — a stack of plates for the publisher, a bank of state cells for the client —
// so the picture reads as a mechanism rather than as a diagram of one.

import type { Beat } from "../model/story";
import type { TraceEvent } from "../model/trace";
import { SequenceBand } from "./SequenceBand";
import { CLIENT_X, LANE_Y, VENUE_X, ease, lerp } from "./geometry";

const STATES = ["synchronising", "live", "recovering", "replaying", "failed"] as const;

interface Props {
  readonly beat: Beat | undefined;
  readonly progress: number;
  readonly trail: readonly Beat[];
  readonly highest: number;
  readonly lines: number;
}

/** A packet, drawn where it has got to. */
function Glyph({ beat, progress }: { readonly beat: Beat; readonly progress: number }) {
  const y = LANE_Y[beat.lane];
  const t = ease(progress);

  // A dropped packet never gets there; it dies part way across, which is the whole of what a loss looks like.
  const stops = beat.fate === "vanish" ? 0.62 : beat.fate === "reject" ? 0.94 : 1;
  const travelled = beat.fate === "lag" ? t * 0.72 : Math.min(t, stops);

  const from = beat.direction === "out" ? VENUE_X : CLIENT_X;
  const to = beat.direction === "out" ? CLIENT_X : VENUE_X;
  const x = lerp(from, to, travelled);

  const dying = beat.fate === "vanish" && t > 0.62;
  const opacity = dying ? Math.max(0, 1 - (t - 0.62) / 0.3) : 1;
  const wide = beat.lane === "snapshot";

  return (
    <g className={`glyph glyph--${beat.tone}`} opacity={opacity} transform={`translate(${x}, ${y})`}>
      {wide ? (
        <rect className="glyph__body glyph__body--wide" x={-26} y={-9} width={52} height={18} />
      ) : beat.lane === "request" ? (
        <path className="glyph__body" d="M -11 0 L 0 -8 L 11 0 L 0 8 Z" />
      ) : (
        <rect className="glyph__body" x={-10} y={-7} width={20} height={14} />
      )}
      {beat.fate === "twin" && (
        <rect className="glyph__body glyph__body--twin" x={-4} y={-11} width={20} height={14} />
      )}
      {dying && (
        <g className="glyph__burst">
          <line x1={-9} y1={-9} x2={9} y2={9} />
          <line x1={9} y1={-9} x2={-9} y2={9} />
        </g>
      )}
      {beat.label !== "" && (
        <text className="glyph__label" x={0} y={-14} textAnchor="middle">
          {beat.label}
        </text>
      )}
    </g>
  );
}

export function Stage({ beat, progress, trail, highest, lines }: Props) {
  const event: TraceEvent | undefined = beat?.event;
  const state = event?.state ?? "synchronising";

  return (
    <g>
      {/* The three tracks. Drawn as rails so a glyph has something to run along. */}
      {(["data", "request", "snapshot"] as const).map((lane) => (
        <g key={lane} className={`rail rail--${lane}`}>
          <line x1={VENUE_X} y1={LANE_Y[lane]} x2={CLIENT_X} y2={LANE_Y[lane]} />
          <text className="rail__label" x={(VENUE_X + CLIENT_X) / 2} y={LANE_Y[lane] - 22} textAnchor="middle">
            {lane === "data" ? "MARKET DATA →" : lane === "request" ? "← RETRANSMIT REQUEST · RESPONSE →" : "← SNAPSHOT REQUEST · REPLY →"}
          </text>
        </g>
      ))}

      {/* The venue: a stack of plates, one per line it publishes on. */}
      <g className="node node--venue" transform={`translate(${VENUE_X}, 0)`}>
        {Array.from({ length: Math.max(1, lines) }, (_, i) => (
          <rect key={i} className="node__plate" x={-44 - i * 5} y={96 - i * 5} width={72} height={40} />
        ))}
        <text className="node__title" x={-8} y={82} textAnchor="middle">
          VENUE
        </text>
        <text className="node__sub" x={-8} y={124} textAnchor="middle">
          {lines > 1 ? `${lines} lines` : "publisher"}
        </text>
        <line className="node__stem" x1={-8} y1={136} x2={-8} y2={LANE_Y.snapshot + 10} />
      </g>

      {/* The client: its state machine as a bank of cells, the live one filled. */}
      <g className="node node--client" transform={`translate(${CLIENT_X}, 0)`}>
        <text className="node__title" x={8} y={82} textAnchor="middle">
          CLIENT
        </text>
        {STATES.map((name, i) => (
          <g key={name} className={`cell ${name === state ? "cell--on" : ""}`}>
            <rect x={-30} y={96 + i * 21} width={78} height={17} />
            <text x={-25} y={108 + i * 21}>
              {name}
            </text>
          </g>
        ))}
        <line className="node__stem" x1={8} y1={82 + 6} x2={8} y2={96} />
      </g>

      {/* Recently arrived packets, fading — so the feed looks like a flow and not a single object. */}
      {trail.map((past, i) => (
        <g key={past.at} opacity={0.1 + 0.12 * (trail.length - i)}>
          <Glyph beat={past} progress={1} />
        </g>
      ))}

      {beat !== undefined && <Glyph beat={beat} progress={progress} />}

      <SequenceBand event={event} highest={highest} />
    </g>
  );
}

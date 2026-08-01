// The order-entry session, drawn as a conversation with a counter down each side.
//
// The whole drawing exists to make one thing visible: the two gutters hold two numbers that are always
// equal, and no arrow between them ever carries the number. SoupBinTCP puts the sequence of a Sequenced
// Data Packet nowhere in the packet, so a client derives it by counting, and agreement is the only
// evidence either side is right.
//
// Static rather than animated, and deliberately. The recovery drawing moves because loss is an *event* and
// motion is the only way to show something failing to arrive. A session is a transcript: it fits on one
// sheet, and the lockstep of the two counters is visible all at once in a way it would not be one rung at a
// time. Hovering a rung says what that step means in words.

import { isNumbered, meaningOf, type SessionTrace, type WireStep } from "../model/session";
import { arrow, boxes, columns, MARGIN, rungY, sheetHeight, SHEET } from "./layout";

interface Props {
  readonly trace: SessionTrace;
  readonly hover: number | undefined;
  readonly onHover: (step: number | undefined) => void;
}

export function Ladder({ trace, hover, onHover }: Props) {
  const steps = trace.steps;
  const height = sheetHeight(steps.length);
  const c = columns();
  const region = boxes(steps.length);
  const body = region.find((b) => b.name === "client")!;

  return (
    <svg
      className="ladder"
      viewBox={`0 0 ${SHEET.w} ${height}`}
      role="img"
      aria-label="an order-entry session, client on the left and exchange on the right"
    >
      {/* The sheet's own frame, matching the recovery drawing: same paper, same hairline. */}
      <rect className="ladder__frame" x={0.5} y={0.5} width={SHEET.w - 1} height={height - 1} />

      <text className="ladder__party" x={c.clientCentre} y={MARGIN.top + 20}>
        A client
      </text>
      <text className="ladder__party" x={c.serverCentre} y={MARGIN.top + 20}>
        The exchange
      </text>
      <text className="ladder__gutter-head" x={c.clientGutterX + COLUMN_GUTTER / 2} y={MARGIN.top + 20}>
        counted
      </text>
      <text className="ladder__gutter-head" x={c.serverGutterX + COLUMN_GUTTER / 2} y={MARGIN.top + 20}>
        assigned
      </text>
      <text className="ladder__gutter-sub" x={c.clientGutterX + COLUMN_GUTTER / 2} y={MARGIN.top + 36}>
        by the client
      </text>
      <text className="ladder__gutter-sub" x={c.serverGutterX + COLUMN_GUTTER / 2} y={MARGIN.top + 36}>
        by the venue
      </text>

      {/* Each party's lifeline. */}
      <line
        className="ladder__life"
        x1={c.clientCentre}
        y1={body.y}
        x2={c.clientCentre}
        y2={body.y + body.h}
      />
      <line
        className="ladder__life"
        x1={c.serverCentre}
        y1={body.y}
        x2={c.serverCentre}
        y2={body.y + body.h}
      />

      {steps.map((step) => (
        <Rung
          key={step.step}
          step={step}
          active={hover === step.step}
          onHover={onHover}
          width={SHEET.w}
        />
      ))}
    </svg>
  );
}

const COLUMN_GUTTER = 92;

interface RungProps {
  readonly step: WireStep;
  readonly active: boolean;
  readonly onHover: (step: number | undefined) => void;
  readonly width: number;
}

function Rung({ step, active, onHover, width }: RungProps) {
  const c = columns();
  const y = rungY(step.step);
  const a = arrow(step.from, step.step);
  const numbered = isNumbered(step);

  return (
    <g
      className={`rung rung--${step.from} ${active ? "is-active" : ""} ${
        numbered ? "is-numbered" : ""
      }`}
      onMouseEnter={() => onHover(step.step)}
      onMouseLeave={() => onHover(undefined)}
      onFocus={() => onHover(step.step)}
      onBlur={() => onHover(undefined)}
      tabIndex={0}
      role="button"
      aria-label={`step ${step.step + 1}: ${step.name}. ${meaningOf(step)}`}
    >
      {/* The hit area, full width, so hovering anywhere on the row works. */}
      <rect className="rung__hit" x={0} y={y - 22} width={width} height={44} />

      {/* The arrow. A Sequenced Data Packet is drawn solid; everything else is drawn light, because the
          numbered stream is the one thing on this sheet with a position. */}
      <line className="rung__arrow" x1={a.x1} y1={a.y} x2={a.x2} y2={a.y} />
      {step.from !== "venue" && (
        <polygon
          className="rung__head"
          points={
            step.from === "client"
              ? `${a.x2 + 10},${a.y - 4} ${a.x2 + 10},${a.y + 4} ${a.x2 + 1},${a.y}`
              : `${a.x2 - 10},${a.y - 4} ${a.x2 - 10},${a.y + 4} ${a.x2 - 1},${a.y}`
          }
        />
      )}

      <text
        className="rung__name"
        x={(a.x1 + a.x2) / 2}
        y={y - 7}
        textAnchor="middle"
      >
        {step.type !== "*" ? `${step.type} · ` : ""}
        {step.name}
      </text>
      <text className="rung__detail" x={(a.x1 + a.x2) / 2} y={y + 12} textAnchor="middle">
        {step.detail}
      </text>

      {/* The two counters. Equal on every rung, and that equality is the argument. */}
      <text className="rung__count" x={c.clientGutterX + COLUMN_GUTTER / 2} y={y + 4}>
        {step.client_next}
      </text>
      <text className="rung__count" x={c.serverGutterX + COLUMN_GUTTER / 2} y={y + 4}>
        {step.server_next}
      </text>

      {/* What the book holds, on the exchange's side, because that is whose book it is. */}
      <text className="rung__book" x={c.serverCentre + 74} y={y + 4} textAnchor="end">
        {step.live_orders > 0 ? `${step.live_orders} live · ${step.shares_open} open` : ""}
      </text>

      {numbered && (
        <text className="rung__seq" x={c.serverCentre - 74} y={y + 4} textAnchor="start">
          #{step.sequence}
        </text>
      )}
    </g>
  );
}

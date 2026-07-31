// The drawing sheet the scene sits on: hairline frame, registration marks, edge ticks, title block.
//
// Borrowed from the architectural-drawing convention rather than from dashboard convention, because a drawing
// sheet tells you where you are without a floating legend covering the thing you came to look at. The frame,
// the ticks and the block are all part of the drawing.

import type { ReactNode } from "react";
import { MARGIN, SHEET, TITLE_BLOCK } from "./geometry";

interface Props {
  readonly children: ReactNode;
  readonly title: string;
  readonly subtitle: string;
  readonly legend: ReadonlyArray<{ readonly swatch: string; readonly label: string }>;
}

/** A corner registration mark, the crosshair a plotter would use to align a sheet. */
function Crosshair({ x, y }: { readonly x: number; readonly y: number }) {
  return (
    <g className="sheet__mark">
      <line x1={x - 7} y1={y} x2={x + 7} y2={y} />
      <line x1={x} y1={y - 7} x2={x} y2={y + 7} />
      <circle cx={x} cy={y} r={3.5} />
    </g>
  );
}

export function Sheet({ children, title, subtitle, legend }: Props) {
  // Edge ticks every 40 units, longer every fifth, so the eye can measure without a ruler.
  const ticks: ReactNode[] = [];
  for (let x = MARGIN; x <= SHEET.width - MARGIN; x += 40) {
    const long = (x - MARGIN) % 200 === 0;
    ticks.push(
      <line key={`t${x}`} className="sheet__tick" x1={x} y1={MARGIN} x2={x} y2={MARGIN + (long ? 9 : 5)} />,
    );
  }
  for (let y = MARGIN; y <= SHEET.height - MARGIN; y += 40) {
    const long = (y - MARGIN) % 200 === 0;
    ticks.push(
      <line key={`l${y}`} className="sheet__tick" x1={MARGIN} y1={y} x2={MARGIN + (long ? 9 : 5)} y2={y} />,
    );
  }

  return (
    <svg
      className="sheet"
      viewBox={`0 0 ${SHEET.width} ${SHEET.height}`}
      role="img"
      aria-label={`${title} — ${subtitle}`}
    >
      <rect className="sheet__ground" x={0} y={0} width={SHEET.width} height={SHEET.height} />
      <rect
        className="sheet__frame"
        x={MARGIN}
        y={MARGIN}
        width={SHEET.width - MARGIN * 2}
        height={SHEET.height - MARGIN * 2}
      />
      {ticks}
      <Crosshair x={MARGIN} y={MARGIN} />
      <Crosshair x={SHEET.width - MARGIN} y={MARGIN} />
      <Crosshair x={MARGIN} y={SHEET.height - MARGIN} />
      <Crosshair x={SHEET.width - MARGIN} y={SHEET.height - MARGIN} />

      {children}

      <g className="sheet__block">
        <rect x={TITLE_BLOCK.x} y={TITLE_BLOCK.y} width={TITLE_BLOCK.width} height={TITLE_BLOCK.height} />
        <line
          x1={TITLE_BLOCK.x}
          y1={TITLE_BLOCK.y + 18}
          x2={TITLE_BLOCK.x + TITLE_BLOCK.width}
          y2={TITLE_BLOCK.y + 18}
        />
        <line
          x1={TITLE_BLOCK.x + 330}
          y1={TITLE_BLOCK.y}
          x2={TITLE_BLOCK.x + 330}
          y2={TITLE_BLOCK.y + TITLE_BLOCK.height}
        />
        <text className="sheet__block-title" x={TITLE_BLOCK.x + 8} y={TITLE_BLOCK.y + 13}>
          {title}
        </text>
        <text className="sheet__block-sub" x={TITLE_BLOCK.x + 8} y={TITLE_BLOCK.y + 33}>
          {subtitle}
        </text>
        {legend.map((entry, at) => (
          <g key={entry.label} transform={`translate(${TITLE_BLOCK.x + 342 + at * 118}, ${TITLE_BLOCK.y + 8})`}>
            <rect className="sheet__swatch" x={0} y={0} width={9} height={9} style={{ fill: entry.swatch }} />
            <text className="sheet__block-legend" x={14} y={8}>
              {entry.label}
            </text>
          </g>
        ))}
      </g>
    </svg>
  );
}

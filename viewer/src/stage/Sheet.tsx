// The drawing sheet everything sits on: hairline frame, registration marks, edge ticks, a drawn title block.
//
// From the architectural-drawing convention rather than the dashboard one, because a drawing sheet tells you
// where you are without a floating legend over the thing you came to look at.

import type { ReactNode } from "react";
import { BLOCK, MARGIN, SHEET } from "./layout";

interface Props {
  readonly children: ReactNode;
  readonly title: string;
  readonly subtitle: string;
  readonly figures: ReadonlyArray<{ readonly label: string; readonly value: string }>;
}

function Crosshair({ x, y }: { readonly x: number; readonly y: number }) {
  return (
    <g className="sheet__mark">
      <line x1={x - 8} y1={y} x2={x + 8} y2={y} />
      <line x1={x} y1={y - 8} x2={x} y2={y + 8} />
      <circle cx={x} cy={y} r={4} />
    </g>
  );
}

export function Sheet({ children, title, subtitle, figures }: Props) {
  const ticks: ReactNode[] = [];
  for (let x = MARGIN; x <= SHEET.w - MARGIN; x += 40) {
    const long = (x - MARGIN) % 200 === 0;
    ticks.push(<line key={`t${x}`} className="sheet__tick" x1={x} y1={MARGIN} x2={x} y2={MARGIN + (long ? 9 : 5)} />);
  }
  for (let y = MARGIN; y <= SHEET.h - MARGIN; y += 40) {
    const long = (y - MARGIN) % 200 === 0;
    ticks.push(<line key={`l${y}`} className="sheet__tick" x1={MARGIN} y1={y} x2={MARGIN + (long ? 9 : 5)} y2={y} />);
  }

  const cell = BLOCK.w / (figures.length + 2);

  return (
    <svg className="sheet" viewBox={`0 0 ${SHEET.w} ${SHEET.h}`} role="img" aria-label={`${title}: ${subtitle}`}>
      <rect className="sheet__ground" x={0} y={0} width={SHEET.w} height={SHEET.h} />
      <rect className="sheet__frame" x={MARGIN} y={MARGIN} width={SHEET.w - MARGIN * 2} height={SHEET.h - MARGIN * 2} />
      {ticks}
      <Crosshair x={MARGIN} y={MARGIN} />
      <Crosshair x={SHEET.w - MARGIN} y={MARGIN} />
      <Crosshair x={MARGIN} y={SHEET.h - MARGIN} />
      <Crosshair x={SHEET.w - MARGIN} y={SHEET.h - MARGIN} />

      {children}

      <g className="sheet__block">
        <rect x={BLOCK.x} y={BLOCK.y} width={BLOCK.w} height={BLOCK.h} />
        <line x1={BLOCK.x} y1={BLOCK.y + 20} x2={BLOCK.x + cell * 2} y2={BLOCK.y + 20} />
        <text className="sheet__block-title" x={BLOCK.x + 10} y={BLOCK.y + 15}>
          {title}
        </text>
        <text className="sheet__block-sub" x={BLOCK.x + 10} y={BLOCK.y + 37}>
          {subtitle}
        </text>
        {figures.map((figure, i) => (
          <g key={figure.label} transform={`translate(${BLOCK.x + cell * (i + 2)}, ${BLOCK.y})`}>
            <line x1={0} y1={0} x2={0} y2={BLOCK.h} />
            <text className="sheet__block-legend" x={11} y={16}>
              {figure.label}
            </text>
            <text className="sheet__block-figure" x={11} y={38}>
              {figure.value}
            </text>
          </g>
        ))}
      </g>
    </svg>
  );
}

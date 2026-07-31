// Where every part of the conversation sits, computed rather than placed.
//
// Same discipline as stage/layout.ts: one source of positions, so `npm run check` can measure the drawing
// before anything renders. Hand-placed coordinates are how the earlier version ended up with a label
// inside the box it described.
//
// A ladder rather than an axonometric stack, because the subject is different. The recovery drawing is
// about three layers of one system, which is a spatial relationship. A session is two parties taking turns,
// which is a temporal one — and the thing worth seeing is the pair of counters staying level down the page
// while no arrow ever carries the number.

export interface Box {
  readonly name: string;
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}

export const SHEET = { w: 1000, h: 0 } as const; // height depends on the step count

export const MARGIN = { x: 28, top: 26, bottom: 24 } as const;

/** What the columns must add up to: the width inside the margins, and nothing else. */
export const CONTENT_WIDTH = SHEET.w - MARGIN.x * 2;

/**
 * The two columns, and the two counter gutters outside them.
 *
 * These sum to exactly `CONTENT_WIDTH`, and the check below enforces it. The first attempt summed to 984
 * inside 944 and pushed the right-hand gutter off the sheet — invisible to a compiler, to a build and to
 * me, which is the whole reason this geometry is measured rather than trusted.
 */
export const COLUMN = {
  clientGutter: 92,
  client: 200,
  span: 360,
  server: 200,
  serverGutter: 92,
} as const;

// A sum that does not match the sheet is a drawing with something off the page, so it fails at import
// rather than rendering something nobody notices is cropped.
const COLUMN_SUM =
  COLUMN.clientGutter + COLUMN.client + COLUMN.span + COLUMN.server + COLUMN.serverGutter;
if (COLUMN_SUM !== CONTENT_WIDTH) {
  throw new Error(`the ladder's columns sum to ${COLUMN_SUM} inside ${CONTENT_WIDTH}`);
}

export const HEAD_HEIGHT = 54;
export const RUNG_HEIGHT = 46;

export function sheetHeight(steps: number): number {
  return MARGIN.top + HEAD_HEIGHT + steps * RUNG_HEIGHT + MARGIN.bottom;
}

interface Columns {
  readonly clientGutterX: number;
  readonly clientX: number;
  readonly clientCentre: number;
  readonly serverX: number;
  readonly serverCentre: number;
  readonly serverGutterX: number;
  readonly spanFrom: number;
  readonly spanTo: number;
}

export function columns(): Columns {
  const clientGutterX = MARGIN.x;
  const clientX = clientGutterX + COLUMN.clientGutter;
  const spanFrom = clientX + COLUMN.client;
  const spanTo = spanFrom + COLUMN.span;
  const serverX = spanTo;
  const serverGutterX = serverX + COLUMN.server;
  return {
    clientGutterX,
    clientX,
    clientCentre: clientX + COLUMN.client / 2,
    serverX,
    serverCentre: serverX + COLUMN.server / 2,
    serverGutterX,
    spanFrom,
    spanTo,
  };
}

/** The y of a rung's centre line. */
export function rungY(step: number): number {
  return MARGIN.top + HEAD_HEIGHT + step * RUNG_HEIGHT + RUNG_HEIGHT / 2;
}

/**
 * The boxes the checker measures: the two party columns, the arrow span, and the two counter gutters.
 *
 * Returned as a flat list rather than a nested structure because every assertion the checker makes is
 * pairwise — no two collide, none escapes the frame — and a tree would have to be walked to ask that.
 */
export function boxes(steps: number): readonly Box[] {
  const c = columns();
  const h = sheetHeight(steps);
  const bodyY = MARGIN.top + HEAD_HEIGHT;
  const bodyH = steps * RUNG_HEIGHT;
  return [
    { name: "clientGutter", x: c.clientGutterX, y: bodyY, w: COLUMN.clientGutter, h: bodyH },
    { name: "client", x: c.clientX, y: bodyY, w: COLUMN.client, h: bodyH },
    { name: "span", x: c.spanFrom, y: bodyY, w: COLUMN.span, h: bodyH },
    { name: "server", x: c.serverX, y: bodyY, w: COLUMN.server, h: bodyH },
    { name: "serverGutter", x: c.serverGutterX, y: bodyY, w: COLUMN.serverGutter, h: bodyH },
    { name: "head", x: MARGIN.x, y: MARGIN.top, w: SHEET.w - MARGIN.x * 2, h: HEAD_HEIGHT },
    { name: "frame", x: 0, y: 0, w: SHEET.w, h },
  ];
}

/** The arrow for a step: where it starts, where it ends, and which way it points. */
export function arrow(from: string, step: number): { x1: number; x2: number; y: number } {
  const c = columns();
  const y = rungY(step);
  if (from === "client") {
    return { x1: c.spanFrom + 6, x2: c.spanTo - 12, y };
  }
  if (from === "server") {
    return { x1: c.spanTo - 6, x2: c.spanFrom + 12, y };
  }
  // The venue's own matching: not an arrow between the parties, so it is drawn as a short mark inside the
  // server's own column rather than crossing the span it never crossed.
  return { x1: c.serverCentre - 26, x2: c.serverCentre + 26, y };
}

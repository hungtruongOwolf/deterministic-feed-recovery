// The drawing, in axonometric projection, with every position computed rather than placed by eye.
//
// Why the three defences are drawn as three stacked planes
// --------------------------------------------------------
// They are layers. Not three options a reader picks between — a ladder that a packet *descends* when the
// layer above it fails. A flat drawing has to say that in words; a stack says it by being a stack, and
// escalation becomes literal downward movement between planes.
//
//   plane 0  two multicast paths, running in parallel across the depth of the plane
//   plane 1  the TCP retransmit service — reached only when both paths above missed
//   plane 2  the snapshot service — reached only when the retransmit came too late
//
// The receiver stands at the right-hand edge of all three, and a column drops through them. That column is
// the escalation: the run is at whichever plane the marker has fallen to.
//
// Axonometric rather than perspective, because this is a drawing sheet: parallel lines stay parallel, a
// length is a length wherever it sits, and nothing is foreshortened into a lie.
//
// Nothing here is a literal. An earlier version placed coordinates by hand and a label ended up inside the
// box it described while the title block ran past the frame — invisible to a compiler and to me. So the
// positions derive from the sheet, and `boxes()` exports what was drawn so a check can measure it.

export interface Box {
  readonly name: string;
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}
export interface Point {
  readonly x: number;
  readonly y: number;
}
/** A point in the drawing's three axes: along the flow, across the plane, and which plane. */
export interface P3 {
  readonly x: number;
  readonly y: number;
  readonly layer: number;
}

export const SHEET = { w: 1280, h: 780 } as const;
export const MARGIN = 30;

const inner = { x: MARGIN + 24, y: MARGIN + 18, w: SHEET.w - (MARGIN + 24) * 2, h: SHEET.h - (MARGIN + 18) * 2 };

// ---------------------------------------------------------------------------
// The projection
// ---------------------------------------------------------------------------

/** How far a step into the plane's depth moves right and up on screen. */
const SHEAR_X = 0.62;
const SHEAR_Y = 0.5;
/** The gap between planes, which is what makes the stack readable as separate layers. */
export const LAYER_DROP = 152;

export const PLANE = { w: 520, d: 158 } as const;
const ORIGIN = { x: inner.x + 34, y: inner.y + 196 } as const;

export function project(p: P3): Point {
  return {
    x: ORIGIN.x + p.x + p.y * SHEAR_X,
    y: ORIGIN.y - p.y * SHEAR_Y + p.layer * LAYER_DROP,
  };
}

/** The four corners of one plane, as a closed path. */
export function planeOutline(layer: number): string {
  const corners: P3[] = [
    { x: 0, y: 0, layer },
    { x: PLANE.w, y: 0, layer },
    { x: PLANE.w, y: PLANE.d, layer },
    { x: 0, y: PLANE.d, layer },
  ];
  return corners.map((c, i) => {
    const p = project(c);
    return `${i === 0 ? "M" : "L"} ${p.x.toFixed(1)} ${p.y.toFixed(1)}`;
  }).join(" ") + " Z";
}

function planeBox(layer: number): Box {
  const cs = [
    project({ x: 0, y: 0, layer }),
    project({ x: PLANE.w, y: 0, layer }),
    project({ x: PLANE.w, y: PLANE.d, layer }),
    project({ x: 0, y: PLANE.d, layer }),
  ];
  const xs = cs.map((c) => c.x);
  const ys = cs.map((c) => c.y);
  const x = Math.min(...xs);
  const y = Math.min(...ys);
  return { name: `plane ${layer}`, x, y, w: Math.max(...xs) - x, h: Math.max(...ys) - y };
}

// ---------------------------------------------------------------------------
// What stands on the planes
// ---------------------------------------------------------------------------

/** Two multicast paths at different depths, so they are visibly two routes and not one drawn twice. */
export const DEPTH = { lineA: 118, lineB: 40, spine: 79 } as const;

const ENGINE_X = 16;
const RECEIVER_X = 492;
export const SWITCH_X = [156, 300] as const;

export interface Size { readonly w: number; readonly h: number }
export const NODE: Size = { w: 76, h: 34 };

/** A node's screen box, centred on its point in the drawing's axes. */
export function nodeBox(name: string, at: P3, size: Size = NODE): Box {
  const p = project(at);
  return { name, x: p.x - size.w / 2, y: p.y - size.h / 2, w: size.w, h: size.h };
}

export const ENGINE = nodeBox("matching engine", { x: ENGINE_X, y: DEPTH.spine, layer: 0 }, { w: 92, h: 40 });
export const RECEIVER = nodeBox("receiver", { x: RECEIVER_X, y: DEPTH.spine, layer: 0 }, { w: 88, h: 46 });
export const SWITCHES = {
  a: SWITCH_X.map((x, i) => nodeBox(`SW·A${i + 1}`, { x, y: DEPTH.lineA, layer: 0 }, { w: 54, h: 22 })),
  b: SWITCH_X.map((x, i) => nodeBox(`SW·B${i + 1}`, { x, y: DEPTH.lineB, layer: 0 }, { w: 54, h: 22 })),
} as const;
export const RETX = nodeBox("retransmit server", { x: 120, y: DEPTH.spine, layer: 1 }, { w: 128, h: 38 });
export const SNAP = nodeBox("snapshot server", { x: 120, y: DEPTH.spine, layer: 2 }, { w: 128, h: 38 });

/** The column the escalation falls down, at the receiver's position, through all three planes. */
export const ESCALATION = [0, 1, 2].map((layer) => project({ x: RECEIVER_X, y: DEPTH.spine, layer }));

// ---------------------------------------------------------------------------
// Routes, in the drawing's axes
// ---------------------------------------------------------------------------

const line = (depth: number): P3[] => [
  { x: ENGINE_X + 46, y: depth, layer: 0 },
  { x: SWITCH_X[0] - 27, y: depth, layer: 0 },
  { x: SWITCH_X[0] + 27, y: depth, layer: 0 },
  { x: SWITCH_X[1] - 27, y: depth, layer: 0 },
  { x: SWITCH_X[1] + 27, y: depth, layer: 0 },
  { x: RECEIVER_X - 30, y: depth, layer: 0 },
];

export const ROUTES = {
  lineA: line(DEPTH.lineA),
  lineB: line(DEPTH.lineB),
  retx: [
    { x: RECEIVER_X, y: DEPTH.spine, layer: 1 },
    { x: 120 + 64, y: DEPTH.spine, layer: 1 },
  ] as P3[],
  snap: [
    { x: RECEIVER_X, y: DEPTH.spine, layer: 2 },
    { x: 120 + 64, y: DEPTH.spine, layer: 2 },
  ] as P3[],
} as const;

export type RouteName = keyof typeof ROUTES;

export function polyline(route: readonly P3[]): string {
  return route.map((p, i) => {
    const s = project(p);
    return `${i === 0 ? "M" : "L"} ${s.x.toFixed(1)} ${s.y.toFixed(1)}`;
  }).join(" ");
}

/** A point `t` of the way along a route, measured on screen so the pace stays even through the projection. */
export function along(route: readonly P3[], t: number, reversed = false): Point {
  const pts = (reversed ? [...route].reverse() : route).map(project);
  const runs: number[] = [];
  for (let i = 1; i < pts.length; i += 1) {
    const a = pts[i - 1] as Point;
    const b = pts[i] as Point;
    runs.push(Math.hypot(b.x - a.x, b.y - a.y));
  }
  const total = runs.reduce((s, r) => s + r, 0);
  if (total === 0) {
    return pts[0] as Point;
  }
  let want = Math.max(0, Math.min(1, t)) * total;
  for (let i = 0; i < runs.length; i += 1) {
    const run = runs[i] as number;
    if (want <= run || i === runs.length - 1) {
      const a = pts[i] as Point;
      const b = pts[i + 1] as Point;
      const f = run === 0 ? 0 : want / run;
      return { x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f };
    }
    want -= run;
  }
  return pts[pts.length - 1] as Point;
}

export function ease(t: number): number {
  const c = Math.max(0, Math.min(1, t));
  return c < 0.5 ? 2 * c * c : 1 - (1 - c) * (1 - c) * 2;
}

// ---------------------------------------------------------------------------
// The book, and the strips
// ---------------------------------------------------------------------------

const stackRight = planeBox(0).x + planeBox(0).w;

export const BOOK = { x: stackRight + 34, y: inner.y + 22, w: inner.x + inner.w - (stackRight + 34), h: 430 } as const;
// The quote sits inside the book region, under the grid. Not a region of its own: the grid and the quote are the
// same subject — what arrived, and what it means — and giving them separate boxes would let the checker approve a
// layout where they had drifted apart.
export const QUOTE = { x: BOOK.x + 4, y: BOOK.y + BOOK.h - 72 } as const;

// Under the quote, because it is the sentence that makes the quote mean something.
export const VERDICT = { x: BOOK.x + 4, y: BOOK.y + BOOK.h - 8 } as const;

export const RAIL = { x: inner.x, y: inner.y + inner.h - 128, w: inner.w, h: 44 } as const;
export const BLOCK = { x: inner.x, y: RAIL.y + RAIL.h + 14, w: inner.w, h: 54 } as const;

export interface GridPlan {
  readonly cell: number;
  readonly cols: number;
  readonly rows: number;
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}

export function gridFor(messages: number): GridPlan {
  const area = { x: BOOK.x + 14, y: BOOK.y + 46, w: BOOK.w - 28, h: BOOK.h - 104 };
  const count = Math.max(1, messages);
  let cell = Math.max(4, Math.floor(Math.sqrt((area.w * area.h) / count)));
  let cols = Math.max(1, Math.floor(area.w / cell));
  let rows = Math.ceil(count / cols);
  while (cell > 4 && rows * cell > area.h) {
    cell -= 1;
    cols = Math.max(1, Math.floor(area.w / cell));
    rows = Math.ceil(count / cols);
  }
  return { cell, cols, rows, x: area.x, y: area.y, w: cols * cell, h: rows * cell };
}

// ---------------------------------------------------------------------------
// What was drawn, so a check can measure it
// ---------------------------------------------------------------------------

export function boxes(messages: number): readonly Box[] {
  const grid = gridFor(messages);
  return [
    planeBox(0),
    planeBox(1),
    planeBox(2),
    { name: "book", ...BOOK },
    { name: "event rail", ...RAIL },
    { name: "title block", ...BLOCK },
    ENGINE,
    RECEIVER,
    ...SWITCHES.a,
    ...SWITCHES.b,
    RETX,
    SNAP,
    { name: "book grid", x: grid.x, y: grid.y, w: grid.w, h: grid.h },
  ];
}

/** Parts that legitimately sit inside a bigger region. */
export const NESTED: ReadonlyArray<readonly [string, string]> = [
  ["matching engine", "plane 0"],
  ["receiver", "plane 0"],
  ["SW·A1", "plane 0"],
  ["SW·A2", "plane 0"],
  ["SW·B1", "plane 0"],
  ["SW·B2", "plane 0"],
  ["retransmit server", "plane 1"],
  ["snapshot server", "plane 2"],
  ["book grid", "book"],
];

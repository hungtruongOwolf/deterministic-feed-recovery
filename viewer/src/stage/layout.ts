// Every position on the sheet, computed from a handful of numbers rather than scattered as literals.
//
// The previous layout was written as magic constants and never measured, so a label sat inside the box it
// described and the title block ran past the frame. Both were invisible to a build and to me. So the rule
// here: nothing is placed by eye, every region derives from the sheet, and `BOXES` exports what was drawn so
// a check can assert that no two of them collide and none escapes the frame.
//
// The drawing is two spaces side by side, because the system has two
// -----------------------------------------------------------------
// **The path** — where a packet physically travels. This is genuinely two-dimensional: an exchange splits
// its feed across two separate networks that run in parallel and rejoin at the receiver, and a third,
// slower TCP path exists for asking questions. Drawn as a plan, redundancy explains itself: the same packet
// is on both arcs, and losing one arc costs nothing.
//
// **The book** — what the client ends up knowing, as a grid with one cell per message. Delivered cells fill
// with ink; missing ones stay cut out. This turns "six messages missing" from a number into a shape.
//
// Damage happens in the first space and shows up as holes in the second, a few beats later. That causal
// link, watched happening, is the whole argument of the project.

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

export const SHEET = { w: 1200, h: 660 } as const;
export const MARGIN = 30;

const inner = {
  x: MARGIN + 22,
  y: MARGIN + 16,
  w: SHEET.w - (MARGIN + 22) * 2,
  h: SHEET.h - (MARGIN + 16) * 2,
} as const;

/** Stacked rows: the ladder over the two plans, then the narration, then the block. */
export const LADDER = { x: inner.x, y: inner.y, w: inner.w, h: 48 } as const;

const mainY = LADDER.y + LADDER.h + 14;
const mainH = 366;

export const PLAN = { x: inner.x, y: mainY, w: 668, h: mainH } as const;
export const BOOK = {
  x: PLAN.x + PLAN.w + 28,
  y: mainY,
  w: inner.x + inner.w - (PLAN.x + PLAN.w + 28),
  h: mainH,
} as const;

export const CAPTION = { x: inner.x, y: mainY + mainH + 18, w: inner.w, h: 46 } as const;
export const BLOCK = { x: inner.x, y: CAPTION.y + CAPTION.h + 14, w: inner.w, h: 52 } as const;

// ---------------------------------------------------------------------------
// Inside the path plan
// ---------------------------------------------------------------------------

const planMid = PLAN.y + 150;

export const ENGINE = { x: PLAN.x + 22, y: planMid - 28, w: 84, h: 56 } as const;
export const RECEIVER = { x: PLAN.x + 520, y: planMid - 52, w: 92, h: 104 } as const;

/** How far the two multicast paths bow away from each other. Separation *is* the redundancy. */
const BOW = 62;
export const LINE_A_Y = planMid - BOW;
export const LINE_B_Y = planMid + BOW;

const switchW = 52;
const switchH = 24;
export const SWITCHES = {
  a: [
    { x: PLAN.x + 190, y: LINE_A_Y - switchH / 2, w: switchW, h: switchH, name: "SW·A1" },
    { x: PLAN.x + 330, y: LINE_A_Y - switchH / 2, w: switchW, h: switchH, name: "SW·A2" },
  ],
  b: [
    { x: PLAN.x + 190, y: LINE_B_Y - switchH / 2, w: switchW, h: switchH, name: "SW·B1" },
    { x: PLAN.x + 330, y: LINE_B_Y - switchH / 2, w: switchW, h: switchH, name: "SW·B2" },
  ],
} as const;

/** The two TCP services, drawn apart from the multicast plan because they are a different network. */
export const RETX = { x: PLAN.x + 120, y: PLAN.y + 292, w: 132, h: 38 } as const;
export const SNAP = { x: PLAN.x + 300, y: PLAN.y + 292, w: 132, h: 38 } as const;

const engineOut: Point = { x: ENGINE.x + ENGINE.w, y: planMid };
const receiverIn: Point = { x: RECEIVER.x, y: planMid };
const receiverFoot: Point = { x: RECEIVER.x + RECEIVER.w / 2, y: RECEIVER.y + RECEIVER.h };

/** A route as a polyline. Interpolation walks it by length, so speed stays even around the bends. */
export const ROUTES = {
  lineA: [
    engineOut,
    { x: PLAN.x + 150, y: LINE_A_Y },
    { x: SWITCHES.a[0].x + switchW, y: LINE_A_Y },
    { x: SWITCHES.a[1].x, y: LINE_A_Y },
    { x: PLAN.x + 460, y: LINE_A_Y },
    receiverIn,
  ],
  lineB: [
    engineOut,
    { x: PLAN.x + 150, y: LINE_B_Y },
    { x: SWITCHES.b[0].x + switchW, y: LINE_B_Y },
    { x: SWITCHES.b[1].x, y: LINE_B_Y },
    { x: PLAN.x + 460, y: LINE_B_Y },
    receiverIn,
  ],
  retx: [receiverFoot, { x: RETX.x + RETX.w + 40, y: RETX.y + RETX.h / 2 }, { x: RETX.x + RETX.w, y: RETX.y + RETX.h / 2 }],
  snap: [receiverFoot, { x: SNAP.x + SNAP.w + 24, y: SNAP.y + SNAP.h / 2 }, { x: SNAP.x + SNAP.w, y: SNAP.y + SNAP.h / 2 }],
} as const;

export type RouteName = keyof typeof ROUTES;

function length(points: readonly Point[]): number[] {
  const runs: number[] = [];
  for (let i = 1; i < points.length; i += 1) {
    const a = points[i - 1] as Point;
    const b = points[i] as Point;
    runs.push(Math.hypot(b.x - a.x, b.y - a.y));
  }
  return runs;
}

/** A point `t` of the way along a route, measured by distance so the pace does not jump at a corner. */
export function along(route: readonly Point[], t: number, reversed = false): Point {
  const points = reversed ? [...route].reverse() : route;
  const runs = length(points);
  const total = runs.reduce((sum, run) => sum + run, 0);
  if (total === 0) {
    return points[0] as Point;
  }
  let want = Math.max(0, Math.min(1, t)) * total;
  for (let i = 0; i < runs.length; i += 1) {
    const run = runs[i] as number;
    if (want <= run || i === runs.length - 1) {
      const a = points[i] as Point;
      const b = points[i + 1] as Point;
      const f = run === 0 ? 0 : want / run;
      return { x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f };
    }
    want -= run;
  }
  return points[points.length - 1] as Point;
}

/** Eased travel, so a packet leaves and lands rather than sliding at a constant rate. */
export function ease(t: number): number {
  const c = Math.max(0, Math.min(1, t));
  return c < 0.5 ? 2 * c * c : 1 - (1 - c) * (1 - c) * 2;
}

export function path(route: readonly Point[]): string {
  return route.map((p, i) => `${i === 0 ? "M" : "L"} ${p.x} ${p.y}`).join(" ");
}

// ---------------------------------------------------------------------------
// Inside the book grid
// ---------------------------------------------------------------------------

export interface GridPlan {
  readonly cell: number;
  readonly cols: number;
  readonly rows: number;
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}

/**
 * Chooses a cell size so `messages` cells fit the book region and stay near-square.
 *
 * Computed rather than chosen, because the message count differs per run and a grid tuned by hand for one
 * of them would spill out of the frame on another — which is exactly the class of mistake the checker in
 * scripts/check.tsx now looks for.
 */
export function gridFor(messages: number): GridPlan {
  const area = { x: BOOK.x + 14, y: BOOK.y + 42, w: BOOK.w - 28, h: BOOK.h - 96 };
  const count = Math.max(1, messages);
  let cell = Math.floor(Math.sqrt((area.w * area.h) / count));
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
    { name: "ladder", ...LADDER },
    { name: "plan", ...PLAN },
    { name: "book", ...BOOK },
    { name: "caption", ...CAPTION },
    { name: "title block", ...BLOCK },
    { name: "engine", ...ENGINE },
    { name: "receiver", ...RECEIVER },
    ...SWITCHES.a.map((s) => ({ name: s.name, x: s.x, y: s.y, w: s.w, h: s.h })),
    ...SWITCHES.b.map((s) => ({ name: s.name, x: s.x, y: s.y, w: s.w, h: s.h })),
    { name: "retransmit server", ...RETX },
    { name: "snapshot server", ...SNAP },
    { name: "book grid", x: grid.x, y: grid.y, w: grid.w, h: grid.h },
  ];
}

/** Regions that may legitimately sit inside another: a part inside the plan it belongs to. */
export const NESTED: ReadonlyArray<readonly [string, string]> = [
  ["engine", "plan"],
  ["receiver", "plan"],
  ["SW·A1", "plan"],
  ["SW·A2", "plan"],
  ["SW·B1", "plan"],
  ["SW·B2", "plan"],
  ["retransmit server", "plan"],
  ["snapshot server", "plan"],
  ["book grid", "book"],
];

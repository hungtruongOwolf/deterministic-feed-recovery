// Where things are on the sheet. One file, so a change to the layout is one diff.

export const SHEET = { width: 1000, height: 470 } as const;

/** The drawn margin: a hairline frame with registration marks, as on a drawing sheet. */
export const MARGIN = 26;

export const VENUE_X = 150;
export const CLIENT_X = 850;

/** The three tracks, and what each one means. */
export const LANE_Y = {
  data: 150,
  request: 216,
  snapshot: 270,
} as const;

export const SEQUENCE = { y: 356, left: 96, right: 904, height: 26 } as const;

export const TITLE_BLOCK = { x: 96, y: 404, width: 808, height: 44 } as const;

/** Eased travel, so a packet leaves and lands rather than sliding at a constant rate. */
export function ease(t: number): number {
  const clamped = Math.max(0, Math.min(1, t));
  return clamped < 0.5 ? 2 * clamped * clamped : 1 - (1 - clamped) * (1 - clamped) * 2;
}

export function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

// Geometry: no two regions collide, nothing escapes the frame, every part sits inside its parent, and every
// label fits the box it is drawn in. Measured once, because it is the same for every run.
//
// The first version of this viewer had a label sitting inside the box it described and a title block that ran
// past the frame. Both were invisible to a compiler, to a build, and to me, and the only honest fix is to
// *measure* the geometry rather than trust it.

import { MARGIN, NESTED, SHEET, boxes, gridFor, type Box } from "../../src/stage/layout";
import { check, contains, overlaps } from "./fixtures";

export function run(): void {
  console.log("geometry");

  const drawn = boxes(300);
  const nested = new Map(NESTED.map(([child, parent]) => [child, parent]));
  const byName = new Map(drawn.map((b) => [b.name, b]));

  let collisions = 0;
  for (let i = 0; i < drawn.length; i += 1) {
    for (let j = i + 1; j < drawn.length; j += 1) {
      const a = drawn[i] as Box;
      const b = drawn[j] as Box;
      if (nested.get(a.name) === b.name || nested.get(b.name) === a.name) {
        continue;
      }
      if (overlaps(a, b)) {
        console.error(`      ${a.name} × ${b.name}`);
        collisions += 1;
      }
    }
  }
  check(collisions === 0, "no two regions overlap");

  const frame: Box = { name: "frame", x: MARGIN, y: MARGIN, w: SHEET.w - MARGIN * 2, h: SHEET.h - MARGIN * 2 };
  const escaped = drawn.filter((b) => !contains(frame, b));
  for (const b of escaped) {
    console.error(`      ${b.name} at y ${b.y}..${b.y + b.h}, x ${b.x}..${b.x + b.w}`);
  }
  check(escaped.length === 0, "nothing escapes the frame");

  let misnested = 0;
  for (const [child, parent] of NESTED) {
    const c = byName.get(child);
    const p = byName.get(parent);
    if (c === undefined || p === undefined || !contains(p, c)) {
      console.error(`      ${child} is not inside ${parent}`);
      misnested += 1;
    }
  }
  check(misnested === 0, "every part sits inside the region it belongs to");

  // Labels are monospace; 0.62em advance is a safe over-estimate for the stacks in theme.css.
  const fits = (text: string, size: number, width: number) => text.length * size * 0.62 <= width;
  const labels: Array<[string, string, number, number]> = [
    ["engine title", "MATCHING", 8, byName.get("matching engine")!.w],
    ["receiver title", "RECEIVER", 8, byName.get("receiver")!.w],
    ["retx title", "RETRANSMIT", 8, byName.get("retransmit server")!.w],
    ["retx sub", "too late in this run", 7, byName.get("retransmit server")!.w],
    ["snap sub", "rebuilds the book", 7, byName.get("snapshot server")!.w],
    ["switch name", "SW·A1", 8, byName.get("SW·A1")!.w],
  ];
  const tight = labels.filter(([, text, size, width]) => !fits(text, size, width));
  for (const [name, text, size, width] of tight) {
    console.error(`      ${name}: "${text}" needs ~${Math.ceil(text.length * size * 0.62)} in ${Math.floor(width)}`);
  }
  check(tight.length === 0, "every label fits the box it is drawn in");

  for (const messages of [40, 120, 300, 900]) {
    const grid = gridFor(messages);
    const region = byName.get("book") as Box;
    const ok = contains(region, { name: "g", x: grid.x, y: grid.y, w: grid.w, h: grid.h }) && grid.cell >= 4;
    check(ok, `the book grid fits its region at ${messages} messages (${grid.cols}×${grid.rows}, cell ${grid.cell})`);
  }
}

// Measuring the drawing, because I cannot look at it.
//
// The first version of this viewer had a label sitting inside the box it described and a title block that
// ran past the frame. Both were invisible to a compiler, to a build, and to me — and the only honest fix is
// to *measure* the geometry rather than trust it. So this does three jobs:
//
//   1. geometry — no two regions collide, nothing escapes the frame, every part sits inside its parent, and
//      every label fits the box it is drawn in;
//   2. rendering — the scene draws, and two progress values of one step produce different markup, so the
//      picture actually moves rather than merely existing;
//   3. language — every step is a sentence, not a field name, and the run that loses data says so in words.
//
// It cannot tell whether the result looks good. Nothing without eyes can, and that limit is stated here
// rather than implied.

import { readFileSync, readdirSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { parseTrace } from "../src/model/trace";
import { buildStory, overture } from "../src/model/story";
import { Sheet } from "../src/stage/Sheet";
import { Stage } from "../src/stage/Stage";
import { MARGIN, NESTED, SHEET, boxes, gridFor, type Box } from "../src/stage/layout";

let failures = 0;
function check(ok: boolean, what: string) {
  if (!ok) {
    console.error("  ✗ " + what);
    failures += 1;
  } else {
    console.log("  ✓ " + what);
  }
}

const overlaps = (a: Box, b: Box) =>
  !(a.y + a.h <= b.y || b.y + b.h <= a.y || a.x + a.w <= b.x || b.x + b.w <= a.x);
const contains = (outer: Box, inner: Box) =>
  inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;

// ---------------------------------------------------------------------------
// 1. geometry — the same for every run, so measured once
// ---------------------------------------------------------------------------

console.log("geometry");
{
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

// ---------------------------------------------------------------------------
// 2 and 3 — per trace
// ---------------------------------------------------------------------------

function frame(trace: ReturnType<typeof parseTrace>, at: number, progress: number): string {
  const story = buildStory(trace);
  const messages = story.reduce((m, b) => Math.max(m, b.event.delivered_through, b.event.end), 1);
  return renderToStaticMarkup(
    <Sheet title="T" subtitle="S" figures={[{ label: "L", value: "1" }]}>
      <Stage trace={trace} beat={story[at]} progress={progress} trail={[]} messages={messages} />
    </Sheet>,
  );
}

for (const file of readdirSync("public/traces").sort()) {
  console.log("\n" + file);
  const trace = parseTrace(readFileSync(`public/traces/${file}`, "utf8"));
  const story = buildStory(trace);

  check(story.length === trace.events.length, "one step per event");
  check(story.every((b) => b.caption.trim().length > 12), "every step is a sentence");
  check(story.every((b) => !/_/.test(b.caption)), "no field names leaked into a caption");
  check(overture(trace).body.length > 80, "the run opens with an explanation");
  check(new Set(story.map((b) => b.caption)).size > 3, "the sentences differ");

  const at = Math.min(6, story.length - 1);
  const drawn = frame(trace, at, 0.5);
  check((drawn.match(/plane__face/g) ?? []).length === 3, "all three defence planes are drawn");
  check(drawn.includes("plan__column"), "the escalation column between the planes is drawn");
  check(drawn.includes('class="plan__route'), "the multicast paths are drawn");
  check(drawn.includes("book__cell--have") || drawn.includes("book__cell--ahead"), "the book grid is drawn");
  check(drawn.includes('class="packet'), "a packet is drawn");
  check(frame(trace, at, 0.1) !== frame(trace, at, 0.9), "the picture moves within a step");

  // The experiment must be visible: a run without the second line draws it struck out.
  const strike = (drawn.match(/plan__strike/g) ?? []).length;
  if (trace.header.lines === 1) {
      check(strike > 0, "the removed defence is drawn struck out");
  } else {
    check(strike === 0, "nothing is struck out when every defence is in place");
  }

  // The run must visibly fall through the layers: an escalating run reaches a lower plane than it starts on.
  const layers = story.map((b) =>
    b.lane === "snapshot" || b.event.state === "failed" || b.event.state === "replaying"
      ? 2
      : b.lane === "request" || b.event.state === "recovering"
        ? 1
        : 0,
  );
  const deepest = Math.max(...layers);
  const expected = trace.summary.snapshot_requests > 0 ? 2 : trace.summary.retransmit_requests > 0 ? 1 : 0;
  check(deepest === expected, `the run falls exactly as far as it should (plane ${deepest})`);

  if (trace.header.mode === "glimpse") {
    const fatal = story.find((b) => b.event.event === "snapshot_rejected");
    check(fatal !== undefined, "the run reaches its rejection");
    check(fatal !== undefined && /never|neither|permanently|refuses/.test(fatal.caption), "the loss is explained in words");
    const lost = fatal === undefined ? 0 : fatal.event.end - fatal.event.first;
    check(lost > 0, `the unrecoverable range is non-empty (${lost} messages)`);
    const late = frame(trace, story.indexOf(fatal!), 0.9);
    check(late.includes("book__cell--hole"), "the holes are visible in the book at that moment");
  }
}

console.log(failures === 0 ? "\nall drawing checks passed" : `\n${failures} drawing checks failed`);
process.exit(failures === 0 ? 0 : 1);

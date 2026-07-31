// A smoke test for the drawing, run in node with no browser.
//
// It cannot tell whether the sheet looks good — nothing without eyes can. What it can tell, and what a
// build alone cannot, is that the scene renders at all, that consecutive frames of the same beat *differ*
// (so the picture moves rather than merely existing), and that the beats a run turns on are present with
// the numbers the trace carries. Those are the three ways this viewer could be silently broken.

import { readFileSync, readdirSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { parseTrace } from "../src/model/trace";
import { buildStory, overture } from "../src/model/story";
import { Sheet } from "../src/stage/Sheet";
import { Stage } from "../src/stage/Stage";

let failures = 0;
function check(ok: boolean, what: string) {
  if (!ok) {
    console.error("  ✗ " + what);
    failures += 1;
  } else {
    console.log("  ✓ " + what);
  }
}

const legend = [{ swatch: "#000", label: "market data" }];

function frame(trace: ReturnType<typeof parseTrace>, at: number, progress: number): string {
  const story = buildStory(trace);
  return renderToStaticMarkup(
    <Sheet title="T" subtitle="S" legend={legend}>
      <Stage
        beat={story[at]}
        progress={progress}
        trail={[]}
        highest={200}
        lines={trace.header.lines}
      />
    </Sheet>,
  );
}

for (const file of readdirSync("public/traces").sort()) {
  console.log("\n" + file);
  const trace = parseTrace(readFileSync(`public/traces/${file}`, "utf8"));
  const story = buildStory(trace);

  check(story.length === trace.events.length, "one beat per event");
  check(
    story.every((b) => b.caption.trim().length > 12),
    "every beat has a sentence, not a field name",
  );
  check(
    story.every((b) => !/_/.test(b.caption)),
    "no snake_case leaked into a caption",
  );
  check(overture(trace).body.length > 80, "the run has an opening explanation");
  check(new Set(story.map((b) => b.caption)).size > 3, "captions are not all the same");

  // Every event's gaps must be inside the span the run reached: a hole drawn past the end of the axis
  // would be a hole nobody could see.
  const highest = story.reduce((m, b) => Math.max(m, b.event.delivered_through, b.event.end), 1);
  check(
    trace.events.every((e) => e.gaps.every(([f, x]) => f <= x && x <= highest + 1)),
    "every drawn hole fits inside the message axis",
  );

  const drawn = frame(trace, Math.min(4, story.length - 1), 0.5);
  check(drawn.includes("<svg"), "the sheet renders");
  check(drawn.includes('class="glyph'), "a packet is drawn");
  check(drawn.includes('class="band__outline"'), "the message axis is drawn");
  check(drawn.includes('class="cell cell--on"') || drawn.includes("cell--on"), "the client's state is lit");

  // The same beat at two progress values must produce different markup, or nothing is moving.
  const early = frame(trace, Math.min(4, story.length - 1), 0.1);
  const late = frame(trace, Math.min(4, story.length - 1), 0.9);
  check(early !== late, "the picture moves within a beat");

  if (trace.header.mode === "glimpse") {
    const fatal = story.find((b) => b.event.event === "snapshot_rejected");
    check(fatal !== undefined, "the glimpse run reaches its rejection");
    check(
      fatal !== undefined && /never|neither|permanently|refuses/.test(fatal.caption),
      "the rejection is explained in words, not codes",
    );
    const lost = fatal === undefined ? 0 : fatal.event.end - fatal.event.first;
    check(lost > 0, `the unfillable range is non-empty (${lost} messages)`);
  }
}

console.log(failures === 0 ? "\nall drawing checks passed" : `\n${failures} drawing checks failed`);
process.exit(failures === 0 ? 0 : 1);

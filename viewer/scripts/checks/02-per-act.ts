// Per trace: rendering, and the escalation each act is forced into.
//
// The picture actually has to move (two progress values of one step must produce different markup), every step
// has to read as a sentence rather than a field name, and a run that reaches its rejection has to say so in
// words.

import { ACTS } from "../../src/model/film";
import { buildStory } from "../../src/model/story";
import { check, deepestLayer, frame, traces } from "./fixtures";

export function run(): void {
  for (const [index, trace] of traces.entries()) {
    const file = ACTS[index]!.file;
    console.log(`\nact ${ACTS[index]!.ordinal}: ${file}`);
    const story = buildStory(trace);

    check(story.length === trace.events.length, "one step per event");
    check(story.every((b) => b.caption.trim().length > 12), "every step is a sentence");
    check(story.every((b) => !/_/.test(b.caption)), "no field names leaked into a caption");

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
    const deepest = deepestLayer(story);
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
}

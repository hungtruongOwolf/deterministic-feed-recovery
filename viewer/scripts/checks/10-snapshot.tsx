// The snapshot rebuild: the story the drawing has to tell (empty, then filling, then identical) and the
// protocol's shape in the order it has to arrive in.

import { readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { parseSnapshot } from "../../src/model/snapshot";
import { SnapshotSection } from "../../src/snapshot/SnapshotSection";
import { check } from "./fixtures";

export function run(): void {
  console.log("\nthe snapshot rebuild");

  const snapshot = parseSnapshot(readFileSync("public/traces/glimpse-snapshot.jsonl", "utf8"));
  const frames = snapshot.frames;

  // The story the drawing has to tell: empty, then filling, then identical.
  check(frames[0]!.bid_levels + frames[0]!.ask_levels === 0, "the client starts with nothing");
  check(!frames[0]!.matches, "and does not claim to match before any state has arrived");
  check(frames[frames.length - 1]!.matches, "and holds the venue's book by the end");
  check(
    frames.some((f) => f.type === "S" && !f.matches),
    "with frames in between where it is part-way there: the convergence a reader watches",
  );

  // The protocol's shape, in the order it has to be in.
  check(frames[0]!.type === "A", "the session opens with a login");
  check(frames[1]!.type === "g", "then begin, before any state, so a wrong service is caught first");
  check(
    frames[frames.length - 2]!.type === "G",
    "the resume sequence arrives second to last, after every level",
  );
  check(frames[frames.length - 1]!.type === "Z", "and the session is closed rather than left hanging");

  // The one number a snapshot protocol exists to deliver, non-zero only once it has been sent.
  check(
    frames.filter((f) => f.resume_from > 0).length === 2,
    "the resume sequence appears only after End Of Snapshot, not before",
  );
  check(
    snapshot.header.resume_from === frames[frames.length - 1]!.resume_from,
    "and the header and the frames agree on it",
  );

  // One frame per level, both sides.
  const levelFrames = frames.filter((f) => f.type === "S").length;
  check(
    levelFrames === snapshot.header.venue_bid_levels + snapshot.header.venue_ask_levels,
    `one frame per level, both sides (${levelFrames} for ${snapshot.header.venue_bid_levels}+${snapshot.header.venue_ask_levels})`,
  );

  const drawn = renderToStaticMarkup(
    <SnapshotSection
      trace={snapshot}
      settings={{ levels: 5, resumeFrom: 4096 }}
      onChange={() => {}}
      live
    />,
  );
  check(/THE VENUE'S BOOK|THE VENUE&#x27;S BOOK/.test(drawn), "the venue's book is drawn");
  check(/THE CLIENT'S BOOK|THE CLIENT&#x27;S BOOK/.test(drawn), "and the client's, beside it");
  check(/is-equal/.test(drawn), "and the drawing says they are equal, rather than leaving it to be noticed");
  check((drawn.match(/class="frame /g) ?? []).length === frames.length, "every frame is listed");
  check(!/NaN|undefined/.test(drawn), "no figure rendered as NaN or undefined");
}

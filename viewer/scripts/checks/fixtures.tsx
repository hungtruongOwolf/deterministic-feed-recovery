// What every check module shares: the reporter, the geometry helpers, and the three acts read once.
//
// Split out of a 1,081-line script that had grown to cover fifteen unrelated concerns in one file, the exact
// thing docs/STYLE.md calls a defect in the library and, until this split, did not check for itself in the
// viewer. Each concern now has its own file under scripts/checks/; this is the part they all needed.
//
// `check`/`report` live in ../reporter.ts and are re-exported here rather than duplicated, since
// scripts/unit.ts needs the same reporter and has no reason to pull in React to get it.

import { readdirSync, readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { ACTS, buildFilm, type Film } from "../../src/model/film";
import { buildStory } from "../../src/model/story";
import { parseTrace, type Trace } from "../../src/model/trace";
import { Sheet } from "../../src/stage/Sheet";
import { Stage } from "../../src/stage/Stage";
import { type Box } from "../../src/stage/layout";

export { check, report } from "../reporter";

export const overlaps = (a: Box, b: Box): boolean =>
  !(a.y + a.h <= b.y || b.y + b.h <= a.y || a.x + a.w <= b.x || b.x + b.w <= a.x);

export const contains = (outer: Box, inner: Box): boolean =>
  inner.x >= outer.x &&
  inner.y >= outer.y &&
  inner.x + inner.w <= outer.x + outer.w &&
  inner.y + inner.h <= outer.y + outer.h;

/** Every .ts/.tsx under a directory, recursively: walked rather than listed, so a new file is never missed. */
export function allSources(dir: string): readonly string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = `${dir}/${entry.name}`;
    if (entry.isDirectory()) {
      out.push(...allSources(path));
    } else if (entry.name.endsWith(".ts") || entry.name.endsWith(".tsx")) {
      out.push(path);
    }
  }
  return out;
}

/** Renders one act's stage at one moment, for checks that need to look at the markup rather than the data. */
export function frame(trace: Trace, at: number, progress: number): string {
  const story = buildStory(trace);
  const messages = story.reduce((m, b) => Math.max(m, b.event.delivered_before, b.event.end), 1);
  return renderToStaticMarkup(
    <Sheet title="T" subtitle="S" figures={[{ label: "L", value: "1" }]}>
      <Stage trace={trace} beat={story[at]} progress={progress} trail={[]} messages={messages} />
    </Sheet>,
  );
}

/** How deep into the stack of defences a run is forced. The number the whole page is about. */
export function deepestLayer(story: ReturnType<typeof buildStory>): number {
  return Math.max(
    ...story.map((b) =>
      b.lane === "snapshot" || b.event.state === "failed" || b.event.state === "replaying"
        ? 2
        : b.lane === "request" || b.event.state === "recovering"
          ? 1
          : 0,
    ),
  );
}

// The three committed runs, read once. Every check module that needs a trace imports it from here rather than
// reading the file again. The original script read order-session.jsonl three times and the benchmark files
// twice, which cost nothing but was never a deliberate choice, just fifteen sections growing independently.
export const traces: readonly Trace[] = ACTS.map((act) =>
  parseTrace(readFileSync(`public/traces/${act.file}`, "utf8")),
);
export const film: Film = buildFilm(traces);

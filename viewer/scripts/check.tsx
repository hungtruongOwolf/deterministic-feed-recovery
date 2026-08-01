// Measuring the drawing, because I cannot look at it.
//
// The first version of this viewer had a label sitting inside the box it described and a title block that
// ran past the frame. Both were invisible to a compiler, to a build, and to me, and the only honest fix is
// to *measure* the geometry rather than trust it. So this does three jobs:
//
//   1. geometry: no two regions collide, nothing escapes the frame, every part sits inside its parent, and
//      every label fits the box it is drawn in;
//   2. rendering: the scene draws, and two progress values of one step produce different markup, so the
//      picture actually moves rather than merely existing;
//   3. language: every step is a sentence, not a field name, and the run that loses data says so in words;
//   4. the argument: the three acts form one continuous film with nothing to choose, and each act falls one
//      layer deeper than the act before it. That last one is the claim the whole page exists to make, so it
//      is asserted rather than hoped for.
//
// It cannot tell whether the result looks good. Nothing without eyes can, and that limit is stated here
// rather than implied.

import { readdirSync, readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { parseTrace } from "../src/model/trace";
import { buildStory } from "../src/model/story";
import {
  ACTS,
  actAt,
  buildFilm,
  DEFAULT_SETTINGS,
  verdictOf,
  prologue,
  runtimeSeconds,
} from "../src/model/film";
import { Controls } from "../src/panels/Controls";
import { Performance } from "../src/panels/Performance";
import { Hero } from "../src/panels/Hero";
import { Findings } from "../src/panels/Findings";
import { Primer } from "../src/panels/Primer";
import { Outcome } from "../src/panels/Outcome";
import { plainly } from "../src/model/plain";
import { fastest, ratePerSecond } from "../src/model/here";
import { FINDINGS, TEST_COUNT } from "../src/model/findings";
import { assertionCosts, NOISE_FLOOR, parseBenchmarks, parseHandoff } from "../src/model/perf";
import { BEATS_PER_SECOND } from "../src/anim/usePlayback";
import { ActStrip } from "../src/panels/ActStrip";
import { meaningOf as meaningFor, parseSession } from "../src/model/session";
import { parseSnapshot } from "../src/model/snapshot";
import { SnapshotSection } from "../src/snapshot/SnapshotSection";
import { SessionSection } from "../src/session/SessionSection";
import {
  arrow as rungArrow,
  boxes as ladderBoxes,
  columns as ladderColumns,
  rungY,
  sheetHeight,
  SHEET as LADDER_SHEET,
} from "../src/session/layout";
import {
  PAGE_TAGLINE,
  PAGE_TITLE,
  PROJECT_ABBREVIATION,
  PROJECT_NAME,
  PROJECT_TAGLINE,
} from "../src/model/brand";
import { Sheet } from "../src/stage/Sheet";
import { Stage } from "../src/stage/Stage";
import { MARGIN, NESTED, SHEET, boxes, gridFor, type Box } from "../src/stage/layout";

// Every .ts/.tsx under a directory, recursively.
function allSources(dir: string): readonly string[] {
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
// 1. geometry: the same for every run, so measured once
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
// 2 and 3: per trace
// ---------------------------------------------------------------------------

function frame(trace: ReturnType<typeof parseTrace>, at: number, progress: number): string {
  const story = buildStory(trace);
  const messages = story.reduce((m, b) => Math.max(m, b.event.delivered_before, b.event.end), 1);
  return renderToStaticMarkup(
    <Sheet title="T" subtitle="S" figures={[{ label: "L", value: "1" }]}>
      <Stage trace={trace} beat={story[at]} progress={progress} trail={[]} messages={messages} />
    </Sheet>,
  );
}

const traces = ACTS.map((act) => parseTrace(readFileSync(`public/traces/${act.file}`, "utf8")));
const film = buildFilm(traces);

/** How deep into the stack of defences a run is forced. The number the whole page is about. */
function deepestLayer(story: ReturnType<typeof buildStory>): number {
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

// ---------------------------------------------------------------------------
// 4. the argument: one film, and it descends
// ---------------------------------------------------------------------------

console.log("\nthe book, which is the point of the message layer");

{
  // The invariant, visible in the committed data rather than only in a test file. Acts I and II lose nothing, so
  // they must end with the *same* book; act III loses data for good, so it must not.
  const finalOf = (t: ReturnType<typeof parseTrace>) => {
    const events = t.events.filter((e) => e.bid_levels > 0 || e.ask_levels > 0);
    return events[events.length - 1];
  };
  const [one, two, three] = traces.map(finalOf);
  check(one !== undefined && two !== undefined && three !== undefined, "every act ends with a book");
  if (one !== undefined && two !== undefined && three !== undefined) {
    check(
      one.best_bid === two.best_bid && one.best_ask === two.best_ask &&
        one.traded_shares === two.traded_shares,
      `acts I and II end with the same book (${one.best_bid / 1e4}/${one.best_ask / 1e4}, ${one.traded_shares} shares)`,
    );
    check(
      three.traded_shares !== one.traded_shares,
      `act III ends with a different book, because data was lost for good (${three.traded_shares} shares against ${one.traded_shares})`,
    );
    check(one.best_bid < one.best_ask, "the book is not crossed at the end of act I");
    check(one.bid_levels > 1 && one.ask_levels > 0, "the book has real depth, not one level");
  }

  // And it draws, and(the part that was missing) it *states the claim* rather than showing two numbers a reader
  // has to interpret. A bid and an ask on a page tell nobody that they are looking at the project's central
  // argument.
  const drawn = frame(traces[0]!, 60, 0.5);
  check(drawn.includes('class="quote'), "the quote is drawn");
  check(/THE BOOK/.test(drawn), "and labelled, so a reader knows what they are looking at");
  check(!/NaN|undefined/.test(drawn), "no book figure rendered as NaN or undefined");

  // The verdict, at the end of each act, where the comparison is meaningful.
  for (const [index, trace] of traces.entries()) {
    const last = trace.events.length - 1;
    const shown = frame(trace, last, 0.9);
    check(/verdict/.test(shown), `act ${index + 1} states a verdict on its book`);
    const expectMatch = index < 2;
    check(
      expectMatch
        ? /THE BOOK IS RIGHT/.test(shown)
        : /THE BOOK IS INCOMPLETE/.test(shown),
      expectMatch
        ? `act ${index + 1} says the book is right, because it is`
        : `act ${index + 1} says the book is incomplete, because it is`,
    );
  }

  // Mid-run it must not claim either way: a difference halfway through is a film that is not over.
  const midway = frame(traces[2]!, 20, 0.5);
  check(/still playing/.test(midway), "mid-run it says the comparison is not due yet rather than reporting a failure");
}

console.log("\nthe film");

check(film.acts.length === 3, "the film has three acts");
check(film.acts[0]!.from === 0, "the film starts at the first act");
check(
  film.acts.every((a, i) => (i === 0 ? true : a.from === film.acts[i - 1]!.to)),
  "the acts are contiguous: no gap and no overlap between them",
);
check(
  film.acts[film.acts.length - 1]!.to === film.moments.length,
  "the acts cover the film exactly",
);
check(
  film.moments.every((m, i) => m.at === i && actAt(film, i).index === m.act),
  "every moment knows which act it is in, and agrees with the lookup",
);
check(prologue(film).body.length > 120, "the film opens with an explanation");

// The claim: each act is forced one layer deeper than the act before it. If this ever stops holding, the
// page is drawing three unrelated runs and calling them an argument.
const depths = traces.map((t) => deepestLayer(buildStory(t)));
check(
  depths.every((d, i) => (i === 0 ? true : d > depths[i - 1]!)),
  `each act falls strictly deeper than the last (${depths.join(" → ")})`,
);
check(depths[0] === 0 && depths[2] === 2, "the film spans the whole stack, first defence to last");

// Nothing to choose: the strip is one bar with three stretches, and there is no select element anywhere.
const strip = renderToStaticMarkup(<ActStrip film={film} position={0} onSeek={() => {}} />);
check(!/<select/.test(strip), "there is no dropdown");
check((strip.match(/strip__bar/g) ?? []).length === 1, "the acts share one bar rather than having one each");
check((strip.match(/strip__act/g) ?? []).length === 3, "all three acts are stretches of it");
check(strip.includes("strip__fill"), "a single fill crosses the act dividers");

// Pacing: long enough that nothing is a blur, short enough that somebody watches it to the end.
const runtime = runtimeSeconds(film, BEATS_PER_SECOND);
check(
  runtime > 90 && runtime < 200,
  `the film runs ${Math.round(runtime)}s at 1×: watchable in one sitting`,
);

const midway = renderToStaticMarkup(
  <ActStrip film={film} position={film.acts[1]!.from + 5} onSeek={() => {}} />,
);
check(midway !== strip, "the strip shows the position moving through the film");
check(/is-past/.test(midway), "an act already played is marked as behind, not as unchosen");

// ---------------------------------------------------------------------------
// 5. the name: a reader has to be able to tell that it is one
// ---------------------------------------------------------------------------

console.log("\nthe name");

check(
  PROJECT_NAME.split(" ").every((word) => word[0] === word[0]!.toUpperCase()),
  `the project name is title case (${PROJECT_NAME})`,
);
check(PROJECT_ABBREVIATION === PROJECT_ABBREVIATION.toUpperCase(), "the abbreviation is upper case");
check(
  PROJECT_ABBREVIATION ===
    PROJECT_NAME.split(" ")
      .map((word) => word[0])
      .join(""),
  "the abbreviation is the initials of the name, so expanding it is obvious",
);

const page = readFileSync("index.html", "utf8");
check(page.includes(PAGE_TITLE), "the browser tab carries the full name");
check(!/<title>[a-z-]+<\/title>/.test(page), "the tab title is not a bare lower-case slug");
check(/name="description"/.test(page), "the page says what it is to anything that only reads the head");

for (const [what, line] of [
  ["the project tagline", PROJECT_TAGLINE],
  ["the page tagline", PAGE_TAGLINE],
] as const) {
  check(!/_/.test(line), `${what} has no field names in it`);
  check(/[.!]$/.test(line.trim()), `${what} is a sentence`);
  check(line.length > 40 && line.length < 220, `${what} is one readable length (${line.length} chars)`);
}

// ---------------------------------------------------------------------------
// 6. the order-entry session: the other direction, and the claim it makes
// ---------------------------------------------------------------------------

console.log("\nthe order-entry session");

const session = parseSession(readFileSync("public/traces/order-session.jsonl", "utf8"));

check(session.steps.length > 8, `the session has ${session.steps.length} recorded steps`);
check(
  session.steps.every((step, i) => step.step === i),
  "the steps are numbered contiguously from zero",
);

// The claim the whole section exists to make. Both numbers come from the file: the server assigned one and
// an independent stream_cursor counted the other, so this asserts agreement rather than computing it.
check(
  session.steps.every((step) => step.client_next === step.server_next || step.step === 0),
  "the two sequence counters agree on every step after the login",
);
check(session.summary.agreed, "the session's own summary says they agreed");
check(session.summary.accounts, "every share liable is executed, canceled or open");
check(
  session.summary.acknowledgements_out === session.summary.server_next - session.header.first_sequence,
  "one acknowledgement per sequence number: no gaps and no reuse",
);

// A numbered packet is only ever from the server: SoupBinTCP's Sequenced Data Packet is the server's alone,
// and a client message carrying a position would mean the recording is wrong about which way an arrow went.
check(
  session.steps.every((step) => step.sequence === 0 || step.from === "server"),
  "only the server's packets carry a position",
);
check(
  session.steps.some((step) => step.from === "client" && step.sequence === 0),
  "the client's own messages carry none",
);

// Geometry, measured the same way as the film's sheet.
{
  const region = ladderBoxes(session.steps.length);
  const frame = region.find((b) => b.name === "frame")!;
  const inner = region.filter((b) => b.name !== "frame");
  let collided = 0;
  for (let i = 0; i < inner.length; i += 1) {
    for (let j = i + 1; j < inner.length; j += 1) {
      const a = inner[i]!;
      const b = inner[j]!;
      // The head sits above the columns, so it is allowed to share their x span but not their rows.
      if (overlaps(a, b)) {
        collided += 1;
        console.error(`    ${a.name} × ${b.name}`);
      }
    }
  }
  check(collided === 0, "no two regions of the ladder overlap");
  for (const b of inner) {
    if (!contains(frame, b)) {
      console.error(`    ${b.name}: x ${b.x}..${b.x + b.w}  y ${b.y}..${b.y + b.h}  (frame ${frame.w}×${frame.h})`);
    }
  }
  check(inner.every((b) => contains(frame, b)), "nothing on the ladder escapes its frame");
  check(
    frame.h === sheetHeight(session.steps.length) && frame.w === LADDER_SHEET.w,
    `the sheet is sized to its content (${LADDER_SHEET.w}×${frame.h} for ${session.steps.length} steps)`,
  );

  // Every rung's arrow has to stay inside the span between the two lifelines, or an arrow would be drawn
  // through the column it is supposed to start at.
  const c = ladderColumns();
  const span = region.find((b) => b.name === "span")!;
  let escaped = 0;
  for (const step of session.steps) {
    const a = rungArrow(step.from, step.step);
    const lo = Math.min(a.x1, a.x2);
    const hi = Math.max(a.x1, a.x2);
    if (lo < span.x || hi > span.x + span.w) {
      if (step.from !== "venue") {
        escaped += 1;
      }
    }
    if (a.y !== rungY(step.step)) {
      escaped += 1;
    }
  }
  check(escaped === 0, "every arrow stays between the two lifelines, on its own rung");

  // Labels have to fit the span they are centred in, or a detail string runs over a lifeline. Measured at
  // the mono advance the stylesheet uses, the same approximation the film's sheet is checked with.
  const MONO_9 = 5.42;
  const MONO_11 = 6.62;
  let overrun = 0;
  for (const step of session.steps) {
    const label = (step.type === "*" ? "" : `${step.type} · `) + step.name;
    const width = Math.max(label.length * MONO_11, step.detail.length * MONO_9);
    if (width > span.w - 24) {
      overrun += 1;
      console.error(`    step ${step.step} "${step.detail}" needs ${Math.round(width)} of ${span.w - 24}`);
    }
  }
  check(overrun === 0, "every rung's label and detail fit the span they are centred in");
  check(c.clientCentre < c.serverCentre, "the client is drawn to the left of the exchange");
}

// Rendering: the drawing exists, the counters are printed, and hovering changes what is said.
{
  const drawn = renderToStaticMarkup(
    <SessionSection
      trace={session}
      settings={{ orders: 3, fill: 40, cancel: true }}
      onChange={() => {}}
      live
    />,
  );
  check(drawn.includes('class="ladder'), "the ladder draws");
  check((drawn.match(/class="rung/g) ?? []).length >= session.steps.length, "every step gets a rung");
  check((drawn.match(/rung__count/g) ?? []).length === session.steps.length * 2, "both counters are printed on every rung");
  check(drawn.includes("rung is-numbered") || drawn.includes("is-numbered"), "the numbered stream is drawn differently");
  check(drawn.includes("session__verdict"), "the verdict on the two counters is stated on the page");
  check(!/<select/.test(drawn), "the session section adds nothing to choose");
  check(/Hover a step/.test(drawn), "the page says how to read it");

  // Language: no field names, and every step means something in words.
  const unexplained = session.steps.filter((step) => {
    const said = meaningFor(step);
    return said.length < 20 || /_/.test(said);
  });
  check(unexplained.length === 0, "every step of the session is explained in plain words");
}

// ---------------------------------------------------------------------------
// 7. the controls: the page runs the library, and says so honestly
// ---------------------------------------------------------------------------

console.log("\nthe controls");

{
  const verdict = verdictOf(film);

  // The two properties that survived a 400-seed sweep, asserted on the committed run as well.
  check(verdict.exactlyOnce, "nothing is delivered twice in any act");
  check(
    verdict.recoveryHeld,
    `nothing is lost until the last defence answers late (${verdict.lost.join(" · ")})`,
  );

  const holding = renderToStaticMarkup(
    <Controls settings={DEFAULT_SETTINGS} onChange={() => {}} busy={false} live verdict={verdict} />,
  );
  const controls_offline_probe = renderToStaticMarkup(
    <Controls
      settings={DEFAULT_SETTINGS}
      onChange={() => {}}
      busy={false}
      live={false}
      verdict={verdict}
    />,
  );
  check(/switched off|WebAssembly/.test(controls_offline_probe), "an inert control says why it is inert");
  // What used to be checked here(that the words "seed" and "faults" appear) is now checked for its
  // *absence*, in the language section below. A control the reader cannot form an intention about is worse than
  // no control, because the page then looks interactive and is not.
  check(holding.includes("is-holding"), "the verdict reads as holding at the committed settings");
  check(!/disabled/.test(holding), "the controls are enabled when the library loaded");
  check(/tendency/.test(holding), "the claim that is only a tendency is labelled as one");

  // With the library unavailable the page must say so and stop pretending the inputs work.
  const offline = renderToStaticMarkup(
    <Controls
      settings={DEFAULT_SETTINGS}
      onChange={() => {}}
      busy={false}
      live={false}
      verdict={verdict}
    />,
  );
  check(/did not load/.test(offline), "a failed load is stated rather than hidden");
  check(/disabled/.test(offline), "and the inputs are disabled rather than inert-looking but live");

  // And a run that broke an invariant has to be reported as broken. This is the case that would matter: it
  // means a bug, and a page that swallowed it would be worse than a page with no verdict at all.
  const broken = renderToStaticMarkup(
    <Controls
      settings={DEFAULT_SETTINGS}
      onChange={() => {}}
      busy={false}
      live
      verdict={{ ...verdict, exactlyOnce: false, duplicated: [0, 2, 0] }}
    />,
  );
  check(broken.includes("is-broken"), "a broken invariant is reported as broken");
  check(/does not hold/.test(broken), "and it is said in words, not only in colour");
}

// ---------------------------------------------------------------------------
// 8. the performance figures: read, never recomputed, and honest about provenance
// ---------------------------------------------------------------------------

console.log("\nthe performance figures");

{
  const perf = {
    shipping: parseBenchmarks(readFileSync("public/bench/results.json", "utf8"), "shipping"),
    paranoid: parseBenchmarks(readFileSync("public/bench/results-paranoid.json", "utf8"), "paranoid"),
    handoff: parseHandoff(readFileSync("public/bench/handoff.json", "utf8"), "handoff"),
  };

  check(perf.shipping.assertions === "off", "the headline figures are the shipping configuration");
  check(perf.shipping.allocations_after_init === 0, "the library allocated nothing after start-up");
  check(perf.shipping.measurements.length >= 5, `${perf.shipping.measurements.length} operations measured`);
  check(
    perf.shipping.measurements.every((m) => m.best_ns > 0 && m.p50_ns >= m.best_ns && m.p99_ns >= m.p50_ns),
    "every measurement is ordered best <= p50 <= p99, so none is a placeholder",
  );
  check(
    perf.shipping.measurements.every((m) => m.unit.length > 8 && !/_/.test(m.unit)),
    "every measurement names what one operation is, in words",
  );

  // The hand-off row that refuses has to actually refuse, or it is a row about a condition that did not occur.
  const refusing = perf.handoff.measurements.filter((h) => h.refused > 0);
  check(refusing.length >= 1, `the slow-consumer row really did fill the ring (${refusing[0]?.refused.toLocaleString() ?? 0} refused)`);

  // Batching must beat one-at-a-time, or the reason pop_batch exists is unsupported by the numbers on the page.
  const single = perf.handoff.measurements.find((h) => h.name === "one at a time");
  const batched = perf.handoff.measurements.find((h) => h.name.includes("batches"));
  check(
    single !== undefined && batched !== undefined && batched.ns_per_message < single.ns_per_message,
    "batched draining is measurably cheaper per message than one at a time",
  );

  // The assertion cost must be labelled as noise where it is noise. This is the check that stops the page
  // quoting a 2% difference as a result.
  const costs = assertionCosts(perf);
  check(costs.length === perf.shipping.measurements.length, "every operation is priced against paranoid");
  check(
    costs.every((c) => c.significant === c.ratio >= NOISE_FLOOR),
    "a difference inside the noise floor is marked as noise, not reported as a win",
  );
  check(costs.some((c) => c.significant), "at least one operation is measurably slower with assertions on");

  // A plausible browser run: three acts of 300 messages in a few milliseconds. Values, not zeroes: a panel
  // rendered with an absent measurement is the fallback branch, and that is checked separately below.
  const here = ratePerSecond({ elapsedMs: 6.4, messages: 900, runs: 2 });
  const drawn = renderToStaticMarkup(<Performance perf={perf} live={here} />);
  // The provenance disclaimer is load-bearing: every other figure on the page is computed in the reader's
  // browser and these are not, so the page must not let anybody assume otherwise.
  check(/Measured natively/.test(drawn) && /not in your browser/.test(drawn), "the panel says where these figures came from");
  check(/batch means/.test(drawn), "the panel says what the percentiles are over");
  check(/tick-to-trade/.test(drawn), "the panel names the latency it does not measure");

  // The one figure on this page a reader causes. Its whole value is that it is not committed, so the checks are
  // about provenance rather than about the number: that it is stated as theirs, that the elapsed time it was
  // derived from is shown so the arithmetic is checkable, and that the gap to native is named rather than left
  // for somebody to notice and mistrust.
  check(here !== undefined, "a timed run becomes a rate");
  check(/in your browser/.test(drawn), "the live figure says whose machine produced it");
  check(/6\.4 ms/.test(drawn) && /900 messages/.test(drawn), "and shows the time and the work behind it");
  check(/fastest of 2 runs since you opened this/.test(drawn), "and says which of the runs it is reporting");

  // Minima, like the native tables. Measured first: back-to-back runs of the same acts came out 2.4× apart with
  // the cold one first, so reporting the latest would make the opening figure the worst one.
  const slowFirst = fastest(undefined, { elapsedMs: 12, messages: 900, runs: 1 });
  const thenFast = fastest(slowFirst, { elapsedMs: 4, messages: 900, runs: 1 });
  check(thenFast.elapsedMs === 4 && thenFast.runs === 2, "a faster run replaces a slower one");
  const thenSlow = fastest(thenFast, { elapsedMs: 30, messages: 900, runs: 1 });
  check(thenSlow.elapsedMs === 4 && thenSlow.runs === 3, "and a slower one does not replace it, but is counted");
  // Per message, because the length control changes how much work a run is.
  const longer = fastest(thenFast, { elapsedMs: 8, messages: 2100, runs: 1 });
  check(longer.messages === 2100, "a longer run that is faster per message wins on the per-message figure");
  const gap = drawn.match(/about (\d+)× slower than the tables below/);
  check(gap !== null, "the gap to the native figures is named");
  check(gap !== null && Number(gap[1]) > 1, `native is faster, by ${gap?.[1] ?? "?"}×, which is the honest direction`);
  check(
    drawn.indexOf("in your browser") < drawn.indexOf("Measured natively"),
    "the number a reader made comes before the numbers I made",
  );
  const withoutWasm = renderToStaticMarkup(<Performance perf={perf} live={undefined} />);
  check(
    /did not load/.test(withoutWasm) && !/messages a second/.test(withoutWasm),
    "and without WebAssembly it says so instead of printing a zero",
  );
  check(!/NaN|undefined|Infinity/.test(drawn), "no figure rendered as NaN, undefined or Infinity");
  check((drawn.match(/class="is-real"|class="is-noise"/g) ?? []).length === costs.length, "every assertion cost is drawn");
  check(/allocations after start-up/.test(drawn), "the allocation count is one of the headline figures");
}

// ---------------------------------------------------------------------------
// 9. the reader's language: no jargon the visitor cannot act on
// ---------------------------------------------------------------------------

console.log("\nthe reader's language");

{
  const verdict = verdictOf(film);
  const controls = renderToStaticMarkup(
    <Controls settings={DEFAULT_SETTINGS} onChange={() => {}} busy={false} live verdict={verdict} />,
  );

  // "seed" was the word this page asked a visitor for, and it is a word they cannot form an intention about.
  // It is still present(reproducibility is the point of the project) but as a footnote naming this pattern.
  check(/>damage</.test(controls), "the damage control is named for what it is");
  check(/>length</.test(controls), "so is the length control");
  check(
    /a little|typical|a lot|brutal/.test(controls),
    "the damage levels are named rather than given as numbers",
  );
  // Stripped of tags, so this is about what a reader *sees* rather than what the markup contains: the word
  // survives in class names and prop names, where it belongs, and must not survive in the text.
  const visible = controls.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ");
  check(!/seed/i.test(visible), "no visible text asks the reader for a \"seed\"");
  // Not "the word never appears": naming the fault injector is informative, and hiding what the thing is
  // called would be a different kind of condescension. The test is that no *control* asks for a count of
  // them: every choice a reader makes is a phrase, and the numbers are behind the phrases.
  const labels = (controls.match(/class="choice__option[^"]*"[^>]*>([^<]+)</g) ?? []).map((m) =>
    m.replace(/.*>/, ""),
  );
  check(labels.length >= 6, `${labels.length} named choices, none of them a raw count`);
  check(
    labels.every((label) => !/\d/.test(label)),
    "no choice a reader makes is expressed as a number",
  );
  check(/names\s+this run|names the pattern/.test(controls), "the number is explained as naming the run");
  check(/same packets go missing at the same moments/.test(controls), "and why it exists is stated, once");

  const session = renderToStaticMarkup(
    <SessionSection
      trace={parseSession(readFileSync("public/traces/order-session.jsonl", "utf8"))}
      settings={{ orders: 3, fill: 40, cancel: true }}
      onChange={() => {}}
      live
    />,
  );
  // The old heading("The other direction: orders coming in") assumed the reader already knew there were two
  // directions. A heading that needs the thing it introduces is not a heading.
  check(!/The other direction/.test(session), "the session panel no longer carries its own heading essay");
  // The heading moved into App's numbered section, so what this panel must still carry is the one claim the
  // drawing is for.
  check(/nowhere in the packet/.test(session), "the panel still states the claim its drawing exists to make");
}

// ---------------------------------------------------------------------------
// 10. the page's weight: the failure mode that got it here
// ---------------------------------------------------------------------------
//
// Every criticism of this page was answered by *adding* an explanation, and the explanations were all true. It
// reached 1,504 visible words across thirteen blocks at the same visual weight: a six-minute read in front of a
// two-minute film. Prose is the thing this project produces most easily and it is the thing a page can least
// afford, so the budget is asserted rather than trusted to taste.
//
// What is counted is what a reader sees *before opening anything*. Folded detail is not a cost: it is the
// mechanism that made the cut possible, so the bodies of <details> are removed before counting.

console.log("\nthe snapshot rebuild");

{
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

console.log("\nkeyboard and assistive technology");

{
  // Symbol-only buttons. `⏮ ◀ ❙❙ ▶` read as nothing to a screen reader, and `title` is not reliably announced, so
  // the transport was a row of unlabelled controls. Measured rather than assumed, because "it looks obvious" is
  // exactly the reasoning that produces this.
  const all = allSources("src")
    .filter((f) => f.endsWith(".tsx"))
    .map((f) => readFileSync(f, "utf8"))
    .join("\n");

  // Every <button> whose children are only symbols or whitespace needs an aria-label.
  const buttons = [...all.matchAll(/<button\b([\s\S]*?)>([\s\S]*?)<\/button>/g)];
  const unlabelled = buttons.filter((m) => {
    const attributes = m[1] ?? "";
    const text = (m[2] ?? "").replace(/\{[^}]*\}/g, "").replace(/&nbsp;/g, " ");
    const hasWords = /[a-z]{3}/i.test(text);
    return !hasWords && !/aria-label/.test(attributes);
  });
  check(
    unlabelled.length === 0,
    `every one of the ${buttons.length} buttons is labelled by its text or by aria-label`,
  );

  // Every input the reader types into needs one too, since the visible label is a sibling span rather than a <label
  // for=>.
  const inputs = [...all.matchAll(/<input\b([\s\S]*?)\/>/g)];
  const bare = inputs.filter((m) => !/aria-label|type="checkbox"/.test(m[1] ?? ""));
  check(bare.length === 0, `every one of the ${inputs.length} inputs is labelled`);
}

console.log("\nthe stylesheet");

{
  // Colour contrast, measured. `--ink-faint` shipped at 2.49:1 while carrying hints, notes and captions, under
  // WCAG AA's 4.5:1 for body text and under even the 3:1 large-text threshold. A lot of the page's explanation was
  // effectively unreadable, and nothing in the build said so.
  //
  // Asserted here so the next person adjusting the palette for looks finds out immediately.
  const theme = readFileSync("src/ui/theme.css", "utf8");
  const colours = new Map(
    [...theme.matchAll(/--([a-z-]+):\s*(#[0-9a-fA-F]{6})/g)].map((m) => [m[1]!, m[2]!]),
  );
  const channel = (v: number) => (v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4);
  const luminance = (hex: string) => {
    const [r, g, b] = [1, 3, 5].map((i) => channel(parseInt(hex.slice(i, i + 2), 16) / 255));
    return 0.2126 * r! + 0.7152 * g! + 0.0722 * b!;
  };
  const page = luminance(colours.get("page") ?? "#ffffff");
  const ratio = (hex: string) => {
    const other = luminance(hex);
    return (Math.max(page, other) + 0.05) / (Math.min(page, other) + 0.05);
  };

  // Everything that carries text. `--rule` is exempt and says why in the stylesheet: it is the hairline of a
  // drawing sheet, and nothing depends on perceiving it.
  const textColours = ["ink", "ink-soft", "ink-faint", "healthy", "fault", "recovery", "unfillable"];
  for (const name of textColours) {
    const hex = colours.get(name);
    if (hex === undefined) {
      continue;
    }
    check(ratio(hex) >= 4.5, `--${name} is ${ratio(hex).toFixed(2)}:1 on the paper (AA needs 4.5)`);
  }
  const strong = colours.get("rule-strong");
  check(
    strong !== undefined && ratio(strong) >= 3,
    `--rule-strong is ${strong === undefined ? "missing" : ratio(strong).toFixed(2)}:1 (non-text needs 3.0)`,
  );
}

{
  // Twenty-one percent of the stylesheet was rules for components deleted in earlier rewrites: whole blocks for a
  // `glyph`, a `band` and a `node` that no longer exist. Dead CSS is invisible: it costs bytes, it makes the file
  // unreadable, and grepping for a class name finds a definition that governs nothing.
  //
  // Dynamic class names are accounted for. `act--${tone}` means `.act--fault` is alive even though the literal never
  // appears, so a checker that only looked for literals would delete working styles.
  const css = ["theme", "layout"].map((n) => readFileSync(`src/ui/${n}.css`, "utf8")).join("\n");
  // Every source file under src/, walked rather than listed.
  //
  // The first version had a hardcoded directory list and reported twenty-four false positives the day a new directory
  // was added, which is precisely the failure a hardcoded list of directories has. A checker that goes stale is worse
  // than no checker, because it teaches people to ignore it.
  const sources = allSources("src").map((f) => readFileSync(f, "utf8")).join("\n");

  const classes = [...new Set([...css.matchAll(/^\.([a-z][a-zA-Z0-9_-]*)/gm)].map((m) => m[1]!))];
  const dead = classes.filter((name) => {
    if (sources.includes(name)) return false;
    const base = name.split("--")[0]!;
    if (name.includes("--") && new RegExp(`${base}--\\$\\{`).test(sources)) return false;
    return !new RegExp(`${base}__\\$\\{`).test(sources);
  });
  check(dead.length === 0, `every one of the ${classes.length} CSS classes is referenced${dead.length > 0 ? `; dead: ${dead.join(", ")}` : ""}`);
}

console.log("\nthe page's weight");

{
  const verdict = verdictOf(film);
  const perf = {
    shipping: parseBenchmarks(readFileSync("public/bench/results.json", "utf8"), "shipping"),
    paranoid: parseBenchmarks(readFileSync("public/bench/results-paranoid.json", "utf8"), "paranoid"),
    handoff: parseHandoff(readFileSync("public/bench/handoff.json", "utf8"), "handoff"),
  };
  const session = parseSession(readFileSync("public/traces/order-session.jsonl", "utf8"));

  const panels: ReadonlyArray<readonly [string, string]> = [
    [
      "controls",
      renderToStaticMarkup(
        <Controls settings={DEFAULT_SETTINGS} onChange={() => {}} busy={false} live verdict={verdict} />,
      ),
    ],
    ["performance", renderToStaticMarkup(<Performance perf={perf} live={ratePerSecond({ elapsedMs: 6.4, messages: 900, runs: 2 })} />)],
    [
      "session",
      renderToStaticMarkup(
        <SessionSection
          trace={session}
          settings={{ orders: 3, fill: 40, cancel: true }}
          onChange={() => {}}
          live
        />,
      ),
    ],
  ];

  // Prose, not data.
  //
  // A table cell and an SVG label are the *content*: the numbers and the drawing are what a reader came for,
  // and counting them would push towards a page that says less about less. What this project over-produces is
  // paragraphs, so paragraphs are what is budgeted: <p>, <li>, and anything in a prose container. Folded bodies
  // are excluded, since folding is the mechanism that made the cut possible.
  const wordsOf = (html: string) => {
    const folded = html.replace(/<div class="fold__body">[\s\S]*?<\/div>/g, " ");
    const prose = (folded.match(/<(?:p|li)\b[^>]*>([\s\S]*?)<\/(?:p|li)>/g) ?? []).join(" ");
    const inline = (folded.match(/class="[^"]*(?:__lede|__note|claim-text|controls__verdict)[^"]*"[^>]*>([\s\S]*?)</g) ?? []).join(" ");
    const text = `${prose} ${inline}`.replace(/<[^>]*>/g, " ").replace(/&[a-z]+;/g, " ").replace(/\s+/g, " ");
    return text.split(" ").filter((w) => /[a-z]{2}/i.test(w)).length;
  };

  let total = 0;
  for (const [name, html] of panels) {
    const words = wordsOf(html);
    total += words;
    check(words <= 80, `${name}: ${words} words of prose before opening anything`);
  }
  check(total <= 180, `${total} words of prose across the three panels, folded`);

  // Every fold has to say what is inside it. "more" promises nothing and is the reason nobody clicks.
  for (const [name, html] of panels) {
    const summaries = (html.match(/class="fold__summary">([^<]+)</g) ?? []).map((m) =>
      m.replace(/.*>/, ""),
    );
    check(
      summaries.every((text) => text.split(" ").length >= 4),
      `${name}: every fold names what is inside it (${summaries.length} folds)`,
    );
  }

  // And no single visible paragraph should be a wall. Forty words is about two lines at this measure.
  for (const [name, html] of panels) {
    const folded = html.replace(/<div class="fold__body">[\s\S]*?<\/div>/g, " ");
    const paragraphs = (folded.match(/<p[^>]*>([\s\S]*?)<\/p>/g) ?? []).map((p) =>
      p.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim(),
    );
    const longest = paragraphs.reduce((most, p) => Math.max(most, p.split(" ").length), 0);
    check(longest <= 45, `${name}: longest visible paragraph is ${longest} words`);
  }
}

// ---------------------------------------------------------------------------
// 11. what a visitor with thirty seconds, and one with five minutes, can see
// ---------------------------------------------------------------------------
//
// Measured before this section existed: 113 passages in the commit history describe a defect found and why it
// hid, and not one appeared on the page outside a collapsed fold. "685 tests" appeared zero times. That is
// designing from what is true about the code, arranged tidily, rather than from what somebody arriving wants.

console.log("\nwhether somebody who does not know the subject can follow it");

{
  // The failure this section exists to prevent: a page that describes what was built, to somebody who already knows
  // why that was worth building. Measured before it existed: the first screen used nine technical terms before
  // defining any of them, every heading was written from the builder's point of view, and nothing said why any of it
  // mattered.
  const primer = renderToStaticMarkup(<Primer onWatch={() => {}} />);
  const words = (html: string) => html.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ");
  const visible = words(primer);

  // No jargon before it has been earned. Each of these is a term a reader would have to already know.
  const jargon = [
    "market-data", "packet", "multicast", "sequence number", "retransmit", "snapshot",
    "MoldUDP64", "SoupBinTCP", "OUCH", "DEEP", "order book", "allocation", "sanitiser",
    "lock-free", "WebAssembly", "C++",
  ];
  const used = jargon.filter((term) => new RegExp(term.replace("+", "\\+"), "i").test(visible));
  check(used.length === 0, `the opening explains itself with no jargon${used.length > 0 ? `; used: ${used.join(", ")}` : ""}`);

  // And it has to say why any of it matters. A mechanism with no stakes is a description.
  check(
    /confidently wrong|does not know|expensive/i.test(visible),
    "the opening says what goes wrong for somebody, not just what goes wrong",
  );
  check(/exchange/i.test(visible) && /prices/i.test(visible), "it names the subject in words anybody has");
  check(visible.split(" ").filter((w) => /[a-z]{2}/i.test(w)).length <= 200, "and stays short enough to read");
  check(/Break it/.test(primer), "there is one obvious thing to press");

  // The plain-language outcome, so the same run is legible without the drawing.
  const settled = traces[0]!.events[traces[0]!.events.length - 1]!;
  const plain = plainly(traces[0]!, settled);
  check(plain.settled, "a finished run reads as an outcome rather than a moment");
  check(plain.good, "and the first act's outcome is a good one, because it is");
  const outcome = words(renderToStaticMarkup(<Outcome outcome={plain} />));
  const jargonInOutcome = jargon.filter((t) => new RegExp(t.replace("+", "\\+"), "i").test(outcome));
  check(jargonInOutcome.length === 0, `the outcome is jargon-free${jargonInOutcome.length > 0 ? `; used: ${jargonInOutcome.join(", ")}` : ""}`);
  check(/identical/.test(outcome), "and states the claim in a word a reader already owns");

  // The failing act has to read as failing, in the same register.
  const broken = plainly(traces[2]!, traces[2]!.events[traces[2]!.events.length - 1]!);
  check(!broken.good, "the act that loses data does not read as a success");
  check(/knows/.test(broken.result), "and says the client knows, which is the point of that act");

  // Headings are questions a reader might have, not statements about what exists.
  const app = readFileSync("src/App.tsx", "utf8");
  const headings = [...app.matchAll(/act-section__number mono">\d<\/span>\s*\n\s*([^<\n]+)/g)].map((m) =>
    m[1]!.trim(),
  );
  check(headings.length === 5, `${headings.length} sections`);
  check(
    headings[0] !== undefined && /notice/.test(headings[0]),
    "the first section a visitor reaches is about what goes wrong, not about the author",
  );
  check(
    headings.findIndex((h) => /while I was building/.test(h)) >= 3,
    "the engineering section comes after the ones anybody can read",
  );
}

console.log("\nwhat a visitor can see");

{
  const hero = renderToStaticMarkup(
    <Hero tests={TEST_COUNT} allocations={0} perPacket="41 ns" live onWatch={() => {}} />,
  );
  const strip = (html: string) => html.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ");

  // Thirty seconds: what it is, that somebody checked it, and where the code is, without pressing anything.
  check(/broken on purpose/.test(hero), "the first line says what was built");
  check(new RegExp(`>${TEST_COUNT}<`).test(hero), `the test count (${TEST_COUNT}) is visible without opening anything`);
  check(/>3</.test(hero) && /compilers/.test(hero), "so is how many compilers check them");
  check(/>0</.test(hero) && /allocations/.test(hero), "and the allocation count");
  check(/github\.com/.test(hero), "the code is one click away from the first screen");
  check(/computed in your browser/.test(hero), "and the page says it is running rather than replaying");
  check(strip(hero).split(" ").filter((w) => /[a-z]{2}/i.test(w)).length <= 90, "the first screen is scannable");

  // Five minutes: the defects, which are the only real evidence of judgement.
  const findings = renderToStaticMarkup(<Findings />);
  check(FINDINGS.length >= 5, `${FINDINGS.length} defects on the page, not in a fold`);
  check(
    FINDINGS.every((f) => f.hid.length > 80 && f.caught.length > 40 && f.matters.length > 40),
    "every one says how it hid, what caught it, and what it changed",
  );
  check(
    FINDINGS.every((f) => !/code review|careful|attention/i.test(f.caught)),
    "none of them credits \"code review\" for catching anything",
  );
  check(
    FINDINGS.some((f) => /12 runs out of 12|12 out of 12/.test(f.caught)),
    "the ThreadSanitizer counterexample is on the page with its numbers",
  );
  // The strongest finding in the repository has to be the first thing an engineer reads, not the fifth.
  check(
    /wrong book/.test(FINDINGS[0]!.title),
    "the hardest finding is first, not buried in the middle",
  );
  check(
    FINDINGS.filter((f) => /assert|seemed obviously true|I had asserted|my own/i.test(f.hid + f.caught)).length >= 1,
    "at least one is framed as the author's own mistake",
  );
  check((findings.match(/class="finding"/g) ?? []).length === FINDINGS.length, "all of them render");
  check(
    FINDINGS.every((f) => f.where.startsWith("https://github.com/")),
    "each links to the code and the reasoning beside it",
  );

  // Per-card cap: this prose is the substance, and it still must not be a wall.
  const longest = FINDINGS.reduce(
    (most, f) => Math.max(most, `${f.hid} ${f.caught} ${f.matters}`.split(" ").length),
    0,
  );
  check(longest <= 130, `the longest defect card is ${longest} words`);
}

console.log(failures === 0 ? "\nall drawing checks passed" : `\n${failures} drawing checks failed`);
process.exit(failures === 0 ? 0 : 1);

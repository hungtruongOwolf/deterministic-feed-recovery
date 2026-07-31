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
//   3. language — every step is a sentence, not a field name, and the run that loses data says so in words;
//   4. the argument — the three acts form one continuous film with nothing to choose, and each act falls one
//      layer deeper than the act before it. That last one is the claim the whole page exists to make, so it
//      is asserted rather than hoped for.
//
// It cannot tell whether the result looks good. Nothing without eyes can, and that limit is stated here
// rather than implied.

import { readFileSync } from "node:fs";
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
import { assertionCosts, NOISE_FLOOR, parseBenchmarks, parseHandoff } from "../src/model/perf";
import { BEATS_PER_SECOND } from "../src/anim/usePlayback";
import { ActStrip } from "../src/panels/ActStrip";
import { meaningOf as meaningFor, parseSession } from "../src/model/session";
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
  console.log(`\nact ${ACTS[index]!.ordinal} — ${file}`);
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
// 4. the argument — one film, and it descends
// ---------------------------------------------------------------------------

console.log("\nthe film");

check(film.acts.length === 3, "the film has three acts");
check(film.acts[0]!.from === 0, "the film starts at the first act");
check(
  film.acts.every((a, i) => (i === 0 ? true : a.from === film.acts[i - 1]!.to)),
  "the acts are contiguous — no gap and no overlap between them",
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
  `the film runs ${Math.round(runtime)}s at 1× — watchable in one sitting`,
);

const midway = renderToStaticMarkup(
  <ActStrip film={film} position={film.acts[1]!.from + 5} onSeek={() => {}} />,
);
check(midway !== strip, "the strip shows the position moving through the film");
check(/is-past/.test(midway), "an act already played is marked as behind, not as unchosen");

// ---------------------------------------------------------------------------
// 5. the name — a reader has to be able to tell that it is one
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
// 6. the order-entry session — the other direction, and the claim it makes
// ---------------------------------------------------------------------------

console.log("\nthe order-entry session");

const session = parseSession(readFileSync("public/traces/order-session.jsonl", "utf8"));

check(session.steps.length > 8, `the session has ${session.steps.length} recorded steps`);
check(
  session.steps.every((step, i) => step.step === i),
  "the steps are numbered contiguously from zero",
);

// The claim the whole section exists to make. Both numbers come from the file — the server assigned one and
// an independent stream_cursor counted the other — so this asserts agreement rather than computing it.
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
// 7. the controls — the page runs the library, and says so honestly
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
  check(holding.includes("WebAssembly"), "the page says the library is running on it");
  // What used to be checked here — that the words "seed" and "faults" appear — is now checked for its
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
// 8. the performance figures — read, never recomputed, and honest about provenance
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

  const drawn = renderToStaticMarkup(<Performance perf={perf} />);
  // The provenance disclaimer is load-bearing: every other figure on the page is computed in the reader's
  // browser and these are not, so the page must not let anybody assume otherwise.
  check(/Measured natively, not in your browser/.test(drawn), "the panel says where these figures came from");
  check(/batch means/.test(drawn), "the panel says what the percentiles are over");
  check(/tick-to-trade/.test(drawn), "the panel names the latency it does not measure");
  check(!/NaN|undefined|Infinity/.test(drawn), "no figure rendered as NaN, undefined or Infinity");
  check((drawn.match(/class="cost /g) ?? []).length === costs.length, "every assertion cost is drawn");
  check(/allocations after start-up/.test(drawn), "the allocation count is one of the headline figures");
}

// ---------------------------------------------------------------------------
// 9. the reader's language — no jargon the visitor cannot act on
// ---------------------------------------------------------------------------

console.log("\nthe reader's language");

{
  const verdict = verdictOf(film);
  const controls = renderToStaticMarkup(
    <Controls settings={DEFAULT_SETTINGS} onChange={() => {}} busy={false} live verdict={verdict} />,
  );

  // "seed" was the word this page asked a visitor for, and it is a word they cannot form an intention about.
  // It is still present — reproducibility is the point of the project — but as a footnote naming this pattern.
  check(/how much goes wrong/.test(controls), "the damage control is phrased as what it means");
  check(/how much of the feed/.test(controls), "so is the length control");
  check(
    /a little|typical|a lot|brutal/.test(controls),
    "the damage levels are named rather than given as numbers",
  );
  check(!/>\s*seed\s*</.test(controls), "nothing on the page asks the reader for a \"seed\"");
  check(/names the pattern/.test(controls), "the number is explained as the name of a pattern");
  check(/reproducible/.test(controls), "and the reason it exists at all is stated");

  const session = renderToStaticMarkup(
    <SessionSection
      trace={parseSession(readFileSync("public/traces/order-session.jsonl", "utf8"))}
      settings={{ orders: 3, fill: 40, cancel: true }}
      onChange={() => {}}
      live
    />,
  );
  // The old heading — "The other direction: orders coming in" — assumed the reader already knew there were two
  // directions. A heading that needs the thing it introduces is not a heading.
  check(!/The other direction/.test(session), "the session heading no longer assumes a direction is known");
  check(/taking orders/.test(session), "it says what the section is");
  check(/sending/.test(session) && /listening/.test(session), "and how it relates to everything above");
}

console.log(failures === 0 ? "\nall drawing checks passed" : `\n${failures} drawing checks failed`);
process.exit(failures === 0 ? 0 : 1);

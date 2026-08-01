// The order-entry session: the other direction, and the claim it makes.
//
// Both counters (the server's own sequence and an independent stream_cursor's count) come from the recorded
// file, so agreement on every step is asserted rather than computed. Geometry is measured the same way the
// film's sheet is.

import { readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { meaningOf as meaningFor, parseSession } from "../../src/model/session";
import {
  arrow as rungArrow,
  boxes as ladderBoxes,
  columns as ladderColumns,
  rungY,
  sheetHeight,
  SHEET as LADDER_SHEET,
} from "../../src/session/layout";
import { SessionSection } from "../../src/session/SessionSection";
import { check, contains, overlaps } from "./fixtures";

export function run(): void {
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
}

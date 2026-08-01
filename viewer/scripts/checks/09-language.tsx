// The reader's language: no jargon the visitor cannot act on.
//
// "seed" was the word this page asked a visitor for, and it is a word they cannot form an intention about. It
// is still present (reproducibility is the point of the project) but as a footnote naming the pattern rather
// than a control.

import { readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { DEFAULT_SETTINGS, verdictOf } from "../../src/model/film";
import { parseSession } from "../../src/model/session";
import { Controls } from "../../src/panels/Controls";
import { SessionSection } from "../../src/session/SessionSection";
import { check, film } from "./fixtures";

export function run(): void {
  console.log("\nthe reader's language");

  const verdict = verdictOf(film);
  const controls = renderToStaticMarkup(
    <Controls settings={DEFAULT_SETTINGS} onChange={() => {}} busy={false} live verdict={verdict} />,
  );

  // "seed" was the word this page asked a visitor for, and it is a word they cannot form an intention about.
  // It is still present (reproducibility is the point of the project) but as a footnote naming this pattern.
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
  // The old heading ("The other direction: orders coming in") assumed the reader already knew there were two
  // directions. A heading that needs the thing it introduces is not a heading.
  check(!/The other direction/.test(session), "the session panel no longer carries its own heading essay");
  // The heading moved into App's numbered section, so what this panel must still carry is the one claim the
  // drawing is for.
  check(/nowhere in the packet/.test(session), "the panel still states the claim its drawing exists to make");
}

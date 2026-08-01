// The page's weight: the failure mode that got it here.
//
// Every criticism of this page was answered by *adding* an explanation, and the explanations were all true. It
// reached 1,504 visible words across thirteen blocks at the same visual weight: a six-minute read in front of a
// two-minute film. Prose is the thing this project produces most easily and it is the thing a page can least
// afford, so the budget is asserted rather than trusted to taste.
//
// What is counted is what a reader sees *before opening anything*. Folded detail is not a cost: it is the
// mechanism that made the cut possible, so the bodies of <details> are removed before counting.

import { readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { DEFAULT_SETTINGS, verdictOf } from "../../src/model/film";
import { ratePerSecond } from "../../src/model/here";
import { parseBenchmarks, parseHandoff } from "../../src/model/perf";
import { parseSession } from "../../src/model/session";
import { Controls } from "../../src/panels/Controls";
import { Performance } from "../../src/panels/Performance";
import { SessionSection } from "../../src/session/SessionSection";
import { check, film } from "./fixtures";

export function run(): void {
  console.log("\nthe page's weight");

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

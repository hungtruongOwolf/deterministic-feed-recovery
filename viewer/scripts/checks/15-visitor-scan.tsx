// What a visitor can see: thirty seconds, and five minutes.
//
// Thirty seconds gets what it is, that somebody checked it, and where the code is, without pressing anything.
// Five minutes gets the defects, which are the only real evidence of judgement, and the strongest one has to
// be the first thing an engineer reads, not the fifth.

import { renderToStaticMarkup } from "react-dom/server";
import { FINDINGS, TEST_COUNT } from "../../src/model/findings";
import { Findings } from "../../src/panels/Findings";
import { Hero } from "../../src/panels/Hero";
import { check } from "./fixtures";

export function run(): void {
  console.log("\nwhat a visitor can see");

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

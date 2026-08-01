// The name: a reader has to be able to tell that it is one.
//
// Title case, an abbreviation that is obviously the initials, and a browser tab that carries the full name
// rather than a bare lower-case slug, which is the smallest possible bar and the one every unnamed side project misses.

import { readFileSync } from "node:fs";
import {
  PAGE_TAGLINE,
  PAGE_TITLE,
  PROJECT_ABBREVIATION,
  PROJECT_NAME,
  PROJECT_TAGLINE,
} from "../../src/model/brand";
import { check } from "./fixtures";

export function run(): void {
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
}

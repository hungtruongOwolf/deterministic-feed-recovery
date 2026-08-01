// Keyboard and assistive technology: symbol-only buttons.
//
// `⏮ ◀ ❙❙ ▶` read as nothing to a screen reader, and `title` is not reliably announced, so the transport was a
// row of unlabelled controls. Measured rather than assumed, because "it looks obvious" is exactly the reasoning
// that produces this.

import { readFileSync } from "node:fs";
import { allSources, check } from "./fixtures";

export function run(): void {
  console.log("\nkeyboard and assistive technology");

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

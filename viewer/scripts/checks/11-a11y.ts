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

  // A visible focus ring. Measured before writing the fix: two :focus-visible rules existed in the whole
  // stylesheet, both scoped to one SVG element each: every real button, input, select and link had no
  // keyboard focus state at all.
  const theme = readFileSync("src/ui/theme.css", "utf8");
  check(
    /button:focus-visible,\s*select:focus-visible,\s*input:focus-visible,\s*a:focus-visible/.test(theme),
    "buttons, selects, inputs and links share one visible focus style",
  );
  check(
    /\[tabindex\]:focus-visible/.test(theme),
    "so do the custom controls built from a div or an SVG group with tabIndex",
  );
  // The colour has to actually be visible, not just present: reusing the contrast helper the stylesheet
  // check already applies to text, since a focus ring nobody can see is the same failure as text nobody can
  // read.
  const channel = (v: number) => (v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4);
  const luminance = (hex: string) => {
    const [r, g, b] = [1, 3, 5].map((i) => channel(parseInt(hex.slice(i, i + 2), 16) / 255));
    return 0.2126 * r! + 0.7152 * g! + 0.0722 * b!;
  };
  const colours = new Map(
    [...theme.matchAll(/--([a-z-]+):\s*(#[0-9a-fA-F]{6})/g)].map((m) => [m[1]!, m[2]!]),
  );
  const page = luminance(colours.get("page") ?? "#ffffff");
  const focusRing = colours.get("recovery");
  check(focusRing !== undefined, "the focus ring uses a real palette colour, not a hardcoded hex");
  if (focusRing !== undefined) {
    const ratio = (Math.max(page, luminance(focusRing)) + 0.05) / (Math.min(page, luminance(focusRing)) + 0.05);
    check(ratio >= 3, `the focus ring is ${ratio.toFixed(2)}:1 on the paper (non-text needs 3.0)`);
  }
}

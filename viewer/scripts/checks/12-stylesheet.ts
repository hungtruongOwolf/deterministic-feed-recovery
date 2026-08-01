// The stylesheet: colour contrast, measured, and dead CSS.
//
// `--ink-faint` shipped at 2.49:1 while carrying hints, notes and captions: under WCAG AA's 4.5:1 for body
// text, and under even the 3:1 large-text threshold. Nothing in the build said so. Twenty-one percent of the
// stylesheet was also rules for components deleted in earlier rewrites, which is invisible for the same reason:
// it costs bytes, and grepping for a class name finds a definition that governs nothing.

import { readFileSync } from "node:fs";
import { allSources, check } from "./fixtures";

export function run(): void {
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
}

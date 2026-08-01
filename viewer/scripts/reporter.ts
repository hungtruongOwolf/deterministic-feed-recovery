// The one thing every check script shares: a way to say pass or fail and keep a tally.
//
// Pulled out of scripts/checks/fixtures.tsx so that scripts/unit.ts (which tests pure functions and has no
// reason to import React) does not have to pull it in just to get this. `report` is a mutable object rather
// than an exported `let`, so every module sees the same counter without depending on how a bundler happens to
// implement live bindings across ES modules.

export const report = { failures: 0 };

export function check(ok: boolean, what: string): void {
  if (!ok) {
    console.error("  ✗ " + what);
    report.failures += 1;
  } else {
    console.log("  ✓ " + what);
  }
}

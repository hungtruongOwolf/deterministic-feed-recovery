// Measuring the drawing, because I cannot look at it.
//
// The first version of this viewer had a label sitting inside the box it described and a title block that
// ran past the frame. Both were invisible to a compiler, to a build, and to me, and the only honest fix is
// to *measure* the geometry rather than trust it. So this does three jobs:
//
//   1. geometry: no two regions collide, nothing escapes the frame, every part sits inside its parent, and
//      every label fits the box it is drawn in;
//   2. rendering: the scene draws, and two progress values of one step produce different markup, so the
//      picture actually moves rather than merely existing;
//   3. language: every step is a sentence, not a field name, and the run that loses data says so in words;
//   4. the argument: the three acts form one continuous film with nothing to choose, and each act falls one
//      layer deeper than the act before it. That last one is the claim the whole page exists to make, so it
//      is asserted rather than hoped for.
//
// It cannot tell whether the result looks good. Nothing without eyes can, and that limit is stated here
// rather than implied.
//
// One file per concern, under scripts/checks/: this used to be 1,081 lines covering fifteen unrelated
// topics, which is the exact thing docs/STYLE.md calls a defect in the library. fixtures.ts holds what every
// section shares (the reporter, the three committed traces, the geometry helpers); everything else is here
// only to call each section in the same order the page would be read in.

import { report } from "./checks/fixtures";
import { run as geometry } from "./checks/01-geometry";
import { run as perAct } from "./checks/02-per-act";
import { run as book } from "./checks/03-book";
import { run as filmChecks } from "./checks/04-film";
import { run as name } from "./checks/05-name";
import { run as orderSession } from "./checks/06-order-session";
import { run as controls } from "./checks/07-controls";
import { run as performance } from "./checks/08-performance";
import { run as language } from "./checks/09-language";
import { run as snapshot } from "./checks/10-snapshot";
import { run as a11y } from "./checks/11-a11y";
import { run as stylesheet } from "./checks/12-stylesheet";
import { run as weight } from "./checks/13-weight";
import { run as visitorPrimer } from "./checks/14-visitor-primer";
import { run as visitorScan } from "./checks/15-visitor-scan";

geometry();
perAct();
book();
filmChecks();
name();
orderSession();
controls();
performance();
language();
snapshot();
a11y();
stylesheet();
weight();
visitorPrimer();
visitorScan();

console.log(
  report.failures === 0 ? "\nall drawing checks passed" : `\n${report.failures} drawing checks failed`,
);
process.exit(report.failures === 0 ? 0 : 1);

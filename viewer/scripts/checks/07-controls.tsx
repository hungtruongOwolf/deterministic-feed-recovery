// The controls: the two properties that survived a 400-seed sweep, and what an inert or broken control says.

import { renderToStaticMarkup } from "react-dom/server";
import { DEFAULT_SETTINGS, verdictOf } from "../../src/model/film";
import { Controls } from "../../src/panels/Controls";
import { check, film } from "./fixtures";

export function run(): void {
  console.log("\nthe controls");

  const verdict = verdictOf(film);

  // The two properties that survived a 400-seed sweep, asserted on the committed run as well.
  check(verdict.exactlyOnce, "nothing is delivered twice in any act");
  check(
    verdict.recoveryHeld,
    `nothing is lost until the last defence answers late (${verdict.lost.join(" · ")})`,
  );

  const holding = renderToStaticMarkup(
    <Controls settings={DEFAULT_SETTINGS} onChange={() => {}} busy={false} live verdict={verdict} />,
  );
  const controls_offline_probe = renderToStaticMarkup(
    <Controls
      settings={DEFAULT_SETTINGS}
      onChange={() => {}}
      busy={false}
      live={false}
      verdict={verdict}
    />,
  );
  check(/switched off|WebAssembly/.test(controls_offline_probe), "an inert control says why it is inert");
  // What used to be checked here (that the words "seed" and "faults" appear) is now checked for its
  // *absence*, in the language section below. A control the reader cannot form an intention about is worse than
  // no control, because the page then looks interactive and is not.
  check(holding.includes("is-holding"), "the verdict reads as holding at the committed settings");
  check(!/disabled/.test(holding), "the controls are enabled when the library loaded");
  check(/tendency/.test(holding), "the claim that is only a tendency is labelled as one");

  // With the library unavailable the page must say so and stop pretending the inputs work.
  const offline = renderToStaticMarkup(
    <Controls
      settings={DEFAULT_SETTINGS}
      onChange={() => {}}
      busy={false}
      live={false}
      verdict={verdict}
    />,
  );
  check(/did not load/.test(offline), "a failed load is stated rather than hidden");
  check(/disabled/.test(offline), "and the inputs are disabled rather than inert-looking but live");

  // And a run that broke an invariant has to be reported as broken. This is the case that would matter: it
  // means a bug, and a page that swallowed it would be worse than a page with no verdict at all.
  const broken = renderToStaticMarkup(
    <Controls
      settings={DEFAULT_SETTINGS}
      onChange={() => {}}
      busy={false}
      live
      verdict={{ ...verdict, exactlyOnce: false, duplicated: [0, 2, 0] }}
    />,
  );
  check(broken.includes("is-broken"), "a broken invariant is reported as broken");
  check(/does not hold/.test(broken), "and it is said in words, not only in colour");
}

// The film: one continuous argument with nothing to choose, and each act forced one layer deeper than the last.
//
// That last claim is the reason this page exists rather than a dropdown of three demos, so it is asserted
// rather than hoped for: the acts are contiguous, the film descends monotonically, and the strip that draws it
// has no <select> anywhere in it.

import { renderToStaticMarkup } from "react-dom/server";
import { BEATS_PER_SECOND } from "../../src/anim/usePlayback";
import { actAt, prologue, runtimeSeconds } from "../../src/model/film";
import { buildStory } from "../../src/model/story";
import { ActStrip } from "../../src/panels/ActStrip";
import { check, deepestLayer, film, traces } from "./fixtures";

export function run(): void {
  console.log("\nthe film");

  check(film.acts.length === 3, "the film has three acts");
  check(film.acts[0]!.from === 0, "the film starts at the first act");
  check(
    film.acts.every((a, i) => (i === 0 ? true : a.from === film.acts[i - 1]!.to)),
    "the acts are contiguous: no gap and no overlap between them",
  );
  check(
    film.acts[film.acts.length - 1]!.to === film.moments.length,
    "the acts cover the film exactly",
  );
  check(
    film.moments.every((m, i) => m.at === i && actAt(film, i).index === m.act),
    "every moment knows which act it is in, and agrees with the lookup",
  );
  check(prologue(film).body.length > 120, "the film opens with an explanation");

  // The claim: each act is forced one layer deeper than the act before it. If this ever stops holding, the
  // page is drawing three unrelated runs and calling them an argument.
  const depths = traces.map((t) => deepestLayer(buildStory(t)));
  check(
    depths.every((d, i) => (i === 0 ? true : d > depths[i - 1]!)),
    `each act falls strictly deeper than the last (${depths.join(" → ")})`,
  );
  check(depths[0] === 0 && depths[2] === 2, "the film spans the whole stack, first defence to last");

  // Nothing to choose: the strip is one bar with three stretches, and there is no select element anywhere.
  const strip = renderToStaticMarkup(<ActStrip film={film} position={0} onSeek={() => {}} />);
  check(!/<select/.test(strip), "there is no dropdown");
  check((strip.match(/strip__bar/g) ?? []).length === 1, "the acts share one bar rather than having one each");
  check((strip.match(/strip__act/g) ?? []).length === 3, "all three acts are stretches of it");
  check(strip.includes("strip__fill"), "a single fill crosses the act dividers");

  // Pacing: long enough that nothing is a blur, short enough that somebody watches it to the end.
  const runtime = runtimeSeconds(film, BEATS_PER_SECOND);
  check(
    runtime > 90 && runtime < 200,
    `the film runs ${Math.round(runtime)}s at 1×: watchable in one sitting`,
  );

  const midway = renderToStaticMarkup(
    <ActStrip film={film} position={film.acts[1]!.from + 5} onSeek={() => {}} />,
  );
  check(midway !== strip, "the strip shows the position moving through the film");
  check(/is-past/.test(midway), "an act already played is marked as behind, not as unchosen");
}

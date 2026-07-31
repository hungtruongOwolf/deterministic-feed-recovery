// The first ten seconds, designed for the person who has them.
//
// The page used to open with a two-minute film behind a play button. A hiring manager with thirty seconds does
// not press play, and until they do the page has told them nothing they can act on — not what was built, not
// how much of it there is, not whether anybody checked it. Meanwhile "685 tests across five sanitiser
// configurations" appeared on the page exactly zero times, which is the single most useful sentence about the
// project.
//
// So: one line saying what it is, four numbers that are hard to argue with, and a link to the code. The film is
// still here and is still the best part — it is just no longer the toll gate.

import { evidence, type Evidence } from "../model/findings";

const REPO = "https://github.com/hungtruongOwolf/deterministic-feed-recovery";

interface Props {
  readonly tests: number;
  readonly allocations: number;
  readonly perPacket: string;
  /** True when the library is running in the visitor's browser rather than being replayed. */
  readonly live: boolean;
  readonly onWatch: () => void;
}

export function Hero({ tests, allocations, perPacket, live, onWatch }: Props) {
  return (
    <section className="hero">
      <p className="hero__what">
        A market-data feed is broken on purpose — packets lost, duplicated, reordered, a line diverging — and a
        C++20 client puts it back together. Then it is asked to prove it did.
      </p>

      <div className="hero__evidence">
        {evidence(tests, allocations, perPacket).map((item: Evidence) => (
          <div key={item.label} className="fact" title={item.title}>
            <span className="fact__value">{item.value}</span>
            <span className="fact__label">{item.label}</span>
          </div>
        ))}
      </div>

      <div className="hero__actions">
        <button className="hero__watch" onClick={onWatch}>
          ▶ &nbsp;Watch a feed break and recover
        </button>
        <a className="hero__repo" href={REPO}>
          read the code on GitHub →
        </a>
        <span className={`hero__live ${live ? "is-live" : ""}`}>
          {live
            ? "everything below is computed in your browser, not replayed"
            : "showing recorded runs — WebAssembly did not load"}
        </span>
      </div>
    </section>
  );
}

// Where you are in the film: one strip, three stretches.
//
// This replaces the chapter buttons, and the difference is not cosmetic. Three buttons are three things to
// pick between, which is what made the acts read as three methods being compared. A strip is one object: the
// acts are stretches of it, the fill crosses the dividers as the film plays, and clicking is seeking rather
// than choosing. Nobody thinks they are comparing three positions in a film.
//
// The dividers are drawn thin and the fill is drawn continuous over them on purpose. The moment the fill
// crosses a divider is the moment the argument lands: it is the same run continuing under worse conditions.

import type { Film } from "../model/film";

interface Props {
  readonly film: Film;
  readonly position: number;
  readonly onSeek: (at: number) => void;
}

export function ActStrip({ film, position, onSeek }: Props) {
  const total = Math.max(1, film.moments.length);
  const played = Math.max(0, Math.min(1, position / total));
  const current = film.acts.findIndex((a) => position < a.to);

  return (
    <nav className="strip" aria-label="position in the film">
      <div className="strip__head mono">
        ONE RUN, THREE TIMES: A DEFENCE REMOVED AT EACH DIVIDER
      </div>

      <div className="strip__bar">
        <div className="strip__fill" style={{ width: `${played * 100}%` }} />
        {film.acts.map((act) => (
          <button
            key={act.file}
            className={`strip__act ${act.index === current ? "is-now" : ""} ${
              act.to <= position ? "is-past" : ""
            }`}
            style={{ flexGrow: act.to - act.from }}
            onClick={() => onSeek(act.from)}
            title={`go back to act ${act.ordinal}: ${act.title}`}
          >
            <span className="strip__ordinal">{act.ordinal}</span>
            <span className="strip__title">{act.title}</span>
            <span className="strip__reaches mono">{act.reaches}</span>
          </button>
        ))}
        <div className="strip__head-mark" style={{ left: `${played * 100}%` }} />
      </div>
    </nav>
  );
}

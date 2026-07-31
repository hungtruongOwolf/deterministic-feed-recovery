// A tick for every moment worth going back to, across the whole film, and clicking one goes back to it.
//
// A scrubber can reach any instant, which sounds better and is worse: the instants that matter are a handful
// out of six hundred, and finding them by dragging is a search. The rail marks them, colours them by what
// they were, and puts the shape of the whole film on one line — three acts' worth, so without playing
// anything you can see that act one is nearly all quiet, act two is a rhythm of loss and repair, and act
// three ends in a colour the other two never reach.
//
// The act dividers sit at the same fractions as the strip above, because it is the same axis. Two timelines
// at different scales would be two arguments.

import type { Film } from "../model/film";

interface Props {
  readonly film: Film;
  readonly at: number;
  readonly onSeek: (at: number) => void;
}

export function EventRail({ film, at, onSeek }: Props) {
  const marks = film.moments.filter((m) => m.beat.notable);
  const span = Math.max(1, film.moments.length - 1);

  return (
    <div className="rail">
      <div className="rail__head mono">
        MOMENTS WORTH GOING BACK TO — click one
        <span className="rail__count">
          {marks.length} of {film.moments.length} steps
        </span>
      </div>
      <div className="rail__track">
        <div className="rail__line" />
        {film.acts.slice(1).map((act) => (
          <div
            key={act.file}
            className="rail__divide"
            style={{ left: `${(act.from / span) * 100}%` }}
          />
        ))}
        <div className="rail__now" style={{ left: `${(at / span) * 100}%` }} />
        {marks.map((mark) => (
          <button
            key={mark.at}
            className={`rail__mark rail__mark--${mark.beat.tone} ${mark.at === at ? "is-now" : ""}`}
            style={{ left: `${(mark.at / span) * 100}%` }}
            onClick={() => onSeek(mark.at)}
            title={`act ${film.acts[mark.act]!.ordinal}, step ${mark.local + 1} — ${mark.beat.caption}`}
            aria-label={`go to act ${film.acts[mark.act]!.ordinal}, step ${mark.local + 1}: ${mark.beat.caption}`}
          />
        ))}
      </div>
      <div className="rail__legend mono">
        <span className="rail__key rail__key--fault" /> damaged
        <span className="rail__key rail__key--repair" /> repairing
        <span className="rail__key rail__key--fatal" /> lost for good
      </div>
    </div>
  );
}

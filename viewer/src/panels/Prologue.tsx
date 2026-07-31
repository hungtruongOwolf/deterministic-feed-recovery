// The card shown before anything moves: what you are about to watch, in two sentences.
//
// Without it the animation is a mechanism nobody asked about. With it, the mechanism is an argument. It
// covers the whole film rather than one run, because the film is the argument and a single run is only a
// third of it.

import { prologue, type Film } from "../model/film";

interface Props {
  readonly film: Film;
  readonly onStart: () => void;
}

export function Prologue({ film, onStart }: Props) {
  const { title, body } = prologue(film);
  const packets = film.acts.reduce((sum, a) => sum + a.trace.header.packets, 0);
  return (
    <div className="overture">
      <div className="overture__card">
        <div className="overture__eyebrow mono">
          3 acts · {packets} packets · {film.moments.length} steps · plays straight through
        </div>
        <h2 className="overture__title">{title}</h2>
        <p className="overture__body">{body}</p>
        <button className="overture__go" onClick={onStart}>
          ▶ &nbsp;Watch it
        </button>
      </div>
    </div>
  );
}

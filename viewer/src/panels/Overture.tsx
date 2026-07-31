// The card shown before anything moves: what you are about to watch, in two sentences.
//
// Without it the animation is a mechanism nobody asked about. With it, the mechanism is an argument.

import type { Trace } from "../model/trace";
import { overture } from "../model/story";

interface Props {
  readonly trace: Trace;
  readonly onStart: () => void;
}

export function Overture({ trace, onStart }: Props) {
  const { title, body } = overture(trace);
  return (
    <div className="overture">
      <div className="overture__card">
        <div className="overture__eyebrow mono">
          seed {trace.header.seed} · {trace.header.packets} packets · {trace.events.length} steps
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

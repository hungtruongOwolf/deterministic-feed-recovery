// The card that carries an act across into the next one.
//
// The three acts used to be labelled by buttons sitting above the drawing, which meant the label for act two
// was visible during act one and said nothing about the transition. Here the sentence appears where the
// transition happens, while the act is already playing underneath it, so "now take the second line away"
// arrives at the moment the second line goes away, and then gets out of the way.

import type { Act } from "../model/film";

interface Props {
  readonly act: Act;
  /** 0 at the first beat of the act, 1 as the card is about to leave. Drives the fade. */
  readonly through: number;
}

export function Interlude({ act, through }: Props) {
  // Full strength for the first half, then fading, so it never covers the drawing during a fault.
  const opacity = through < 0.5 ? 1 : Math.max(0, 1 - (through - 0.5) * 2);
  return (
    <div className="interlude" style={{ opacity }} aria-live="polite">
      <div className="interlude__card">
        <span className="interlude__ordinal mono">ACT {act.ordinal}</span>
        <h2 className="interlude__title">{act.title}</h2>
        <p className="interlude__body">{act.change}</p>
      </div>
    </div>
  );
}

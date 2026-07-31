// The card that appears on a beat worth stopping for.
//
// The counterpart to the caption strip: the strip narrates every step, this interrupts for the ones that
// change the story. Kept to one sentence of consequence and the numbers behind it.

import type { Beat } from "../model/story";

interface Props {
  readonly beat: Beat | undefined;
}

export function ActCard({ beat }: Props) {
  if (beat === undefined || !beat.notable) {
    return null;
  }
  const { event } = beat;
  return (
    <aside className={`act act--${beat.tone}`} key={beat.at}>
      <div className="act__eyebrow mono">
        step {beat.at + 1} · {event.layer} · {event.event.replace(/_/g, " ")}
        {event.reason !== "ok" ? ` · ${event.reason.replace(/_/g, " ")}` : ""}
      </div>
      <p className="act__body">{beat.caption}</p>
      <div className="act__figures mono">
        <span>delivered through {event.delivered_through}</span>
        <span>missing {event.missing}</span>
        <span>holes {event.holes}</span>
      </div>
    </aside>
  );
}

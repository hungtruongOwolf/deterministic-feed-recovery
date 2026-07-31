// A tick for every moment worth going back to, and clicking one goes back to it.
//
// A scrubber can reach any instant, which sounds better and is worse: the instants that matter are a handful
// out of two hundred, and finding them by dragging is a search. The rail marks them, colours them by what
// they were, and puts the whole shape of the run on one line — you can see at a glance that a run is three
// losses and a recovery, or one loss and a collapse.

import type { Beat } from "../model/story";

interface Props {
  readonly story: readonly Beat[];
  readonly at: number;
  readonly onSeek: (at: number) => void;
}

export function EventRail({ story, at, onSeek }: Props) {
  const marks = story.filter((b) => b.notable);
  const span = Math.max(1, story.length - 1);

  return (
    <div className="rail">
      <div className="rail__head mono">
        MOMENTS WORTH GOING BACK TO — click one
        <span className="rail__count">{marks.length} of {story.length} steps</span>
      </div>
      <div className="rail__track">
        <div className="rail__line" />
        <div className="rail__now" style={{ left: `${(at / span) * 100}%` }} />
        {marks.map((mark) => (
          <button
            key={mark.at}
            className={`rail__mark rail__mark--${mark.tone} ${mark.at === at ? "is-now" : ""}`}
            style={{ left: `${(mark.at / span) * 100}%` }}
            onClick={() => onSeek(mark.at)}
            title={`step ${mark.at + 1} — ${mark.caption}`}
            aria-label={`go to step ${mark.at + 1}: ${mark.caption}`}
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

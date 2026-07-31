// The controls: the thing that was missing. Play, pause, step, speed, and a timeline you can drag.

import type { Playback, Speed } from "../anim/usePlayback";

const SPEEDS: readonly Speed[] = [0.5, 1, 2, 4];

interface Props {
  readonly playback: Playback;
  readonly beats: number;
  readonly caption: string;
}

export function Transport({ playback, beats, caption }: Props) {
  const at = Math.min(beats - 1, Math.floor(playback.position));

  return (
    <div className="transport">
      <div className="transport__row">
        <button className="transport__key" onClick={playback.restart} title="Back to the start" aria-label="Back to the start">
          ⏮
        </button>
        <button className="transport__key" onClick={() => playback.step(-1)} title="Previous step" aria-label="Previous step">
          ◀
        </button>
        <button
          className="transport__key transport__key--main"
          onClick={playback.toggle}
          title={playback.playing ? "Pause" : "Play"}
          aria-label={playback.playing ? "Pause" : "Play"}
        >
          {playback.playing ? "❙❙" : "▶"}
        </button>
        <button className="transport__key" onClick={() => playback.step(1)} title="Next step" aria-label="Next step">
          ▶
        </button>

        <div className="transport__speeds">
          {SPEEDS.map((rate) => (
            <button
              key={rate}
              className={`transport__speed ${playback.speed === rate ? "is-on" : ""}`}
              onClick={() => playback.setSpeed(rate)}
              aria-label={`play at ${rate} times speed`}
              aria-pressed={playback.speed === rate}
            >
              {rate}×
            </button>
          ))}
        </div>

        <span className="transport__count mono">
          step {at + 1} / {beats}
        </span>
      </div>

      <input
        className="transport__scrub"
        type="range"
        min={0}
        max={Math.max(0, beats - 1)}
        step={0.01}
        value={playback.position}
        onChange={(e) => playback.seek(Number(e.currentTarget.value))}
        aria-label="position in the run"
      />

      <p className="transport__caption">{caption}</p>
    </div>
  );
}

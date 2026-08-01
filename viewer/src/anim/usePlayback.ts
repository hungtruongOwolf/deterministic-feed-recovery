// The playback clock: a position in beats, advanced by requestAnimationFrame.
//
// Position is fractional on purpose. The whole number selects which beat is current; the fraction is how far
// its glyph has travelled. That is what makes the picture *move* between two states rather than snap between
// them, and motion is the only thing that shows a packet being lost as an event rather than as an absence.

import { useCallback, useEffect, useRef, useState } from "react";

export type Speed = 0.5 | 1 | 2 | 4;

export interface Playback {
  /** Fractional beat position: `Math.floor` is the current beat, the remainder is its progress. */
  readonly position: number;
  readonly playing: boolean;
  readonly speed: Speed;
  readonly play: () => void;
  readonly pause: () => void;
  readonly toggle: () => void;
  readonly restart: () => void;
  readonly seek: (position: number) => void;
  readonly step: (delta: number) => void;
  readonly setSpeed: (speed: Speed) => void;
}

/** Beats per second at 1×, before pacing. */
export const BEATS_PER_SECOND = 2.4;

/**
 * How long to hold each beat, relative to the others.
 *
 * Uniform pacing was fine for one run of 250 beats and is wrong for a film of 623: five minutes of even
 * ticking, most of it heartbeats. A film paces itself: the quiet stretches go past quickly and the moments
 * that matter are held long enough to read. Nothing is skipped, so the position axis still means what it
 * says; only the speed along it varies.
 *
 * Supplied by the caller because pacing is a property of the story, and this hook knows only about time.
 */
export type Dwell = (beat: number) => number;

const EVEN: Dwell = () => 1;

export function usePlayback(beats: number, dwell: Dwell = EVEN): Playback {
  const [position, setPosition] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState<Speed>(1);

  // Held in refs so the animation callback never needs to be rebuilt, which would restart the frame loop and
  // make playback stutter every time a caption changed.
  const last = useRef<number | null>(null);
  const live = useRef({ playing, speed, beats, dwell });
  live.current = { playing, speed, beats, dwell };

  useEffect(() => {
    let frame = 0;
    const tick = (now: number) => {
      const previous = last.current;
      last.current = now;
      const { playing: running, speed: rate, beats: total, dwell: hold } = live.current;
      if (running && previous !== null && total > 0) {
        const seconds = Math.min(0.25, (now - previous) / 1000);
        setPosition((current) => {
          // A long dwell means fewer beats per second, so divide rather than multiply.
          const held = Math.max(0.05, hold(Math.floor(current)));
          const next = current + (seconds * BEATS_PER_SECOND * rate) / held;
          if (next >= total - 1) {
            setPlaying(false);
            return total - 1;
          }
          return next;
        });
      }
      frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, []);

  const play = useCallback(() => {
    last.current = null;
    setPosition((current) => (beats > 0 && current >= beats - 1 ? 0 : current));
    setPlaying(true);
  }, [beats]);

  const pause = useCallback(() => setPlaying(false), []);
  const toggle = useCallback(() => (live.current.playing ? pause() : play()), [pause, play]);

  const restart = useCallback(() => {
    setPosition(0);
    last.current = null;
  }, []);

  const seek = useCallback(
    (next: number) => {
      setPlaying(false);
      setPosition(Math.max(0, Math.min(beats > 0 ? beats - 1 : 0, next)));
    },
    [beats],
  );

  const step = useCallback(
    (delta: number) => {
      setPlaying(false);
      // Snap to a whole beat, so stepping lands on a state rather than half way through a movement.
      setPosition((current) =>
        Math.max(0, Math.min(beats > 0 ? beats - 1 : 0, Math.round(current) + delta)),
      );
    },
    [beats],
  );

  return { position, playing, speed, play, pause, toggle, restart, seek, step, setSpeed };
}

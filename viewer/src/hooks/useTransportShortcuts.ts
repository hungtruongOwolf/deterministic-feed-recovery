// Space to play, arrows to step: the shortcuts anybody tries on something that moves.

import { useEffect } from "react";
import { type Playback } from "../anim/usePlayback";

export function useTransportShortcuts(playback: Playback): void {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement) {
        return;
      }
      if (e.code === "Space") {
        e.preventDefault();
        playback.toggle();
      } else if (e.code === "ArrowRight") {
        playback.step(1);
      } else if (e.code === "ArrowLeft") {
        playback.step(-1);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [playback]);
}

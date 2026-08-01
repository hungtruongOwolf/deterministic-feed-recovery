// The library itself, and the film it produces.
//
// Loads the WebAssembly engine once, falling back to the committed traces if it cannot be: a reader on a
// browser without WebAssembly should see the argument, not an error, and the controls say plainly that they
// are inert rather than pretending to work. Once the engine is here, every act re-runs whenever the settings
// change, synchronous on purpose, since a run of three hundred messages is a few milliseconds and a worker
// would add a message protocol to save nothing anybody can perceive.
//
// Split out of App.tsx, which had grown past 400 lines by holding four independent data sources' worth of
// effects in one component. This one owns the film itself; useOrderSession, useGlimpseSnapshot and
// usePerformanceData each own the other three.

import { useEffect, useState } from "react";
import { buildFilm, type Film, type Settings } from "../model/film";
import { fastest, type LiveRun } from "../model/here";
import { parseTrace, TraceFormatError, type Trace } from "../model/trace";
import { loadEngine, type Engine } from "../wasm/engine";
import { ACTS } from "../model/film";

interface EngineTraces {
  readonly engine: Engine | undefined;
  readonly film: Film | undefined;
  readonly error: string | undefined;
  readonly busy: boolean;
  /** The one measurement on the page the reader causes. See model/here.ts for what it is and is not. */
  readonly live: LiveRun | undefined;
}

export function useEngineTraces(settings: Settings, onRegenerated: () => void): EngineTraces {
  const [film, setFilm] = useState<Film | undefined>();
  const [engine, setEngine] = useState<Engine | undefined>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | undefined>();
  const [live, setLive] = useState<LiveRun | undefined>(undefined);

  useEffect(() => {
    let cancelled = false;
    loadEngine()
      .then((loaded) => {
        if (!cancelled) {
          setEngine(loaded);
        }
      })
      .catch(() => {
        if (cancelled) {
          return;
        }
        // Fall back to what is committed, which is the same three runs at the default settings.
        Promise.all(
          ACTS.map(async (act) => {
            const response = await fetch(`traces/${act.file}`);
            if (!response.ok) {
              throw new Error(`traces/${act.file}: ${response.status}`);
            }
            return parseTrace(await response.text());
          }),
        )
          .then((traces: readonly Trace[]) => {
            if (!cancelled) {
              setFilm(buildFilm(traces));
              setError(undefined);
            }
          })
          .catch((cause: unknown) => {
            if (!cancelled) {
              setError(cause instanceof TraceFormatError ? cause.message : String(cause));
            }
          });
      });
    return () => {
      cancelled = true;
    };
  }, []);

  // Every act re-run whenever the settings change. Synchronous on purpose: a run of three hundred messages
  // is a few milliseconds, and a worker would add a message protocol to save nothing anybody can perceive.
  useEffect(() => {
    if (engine === undefined) {
      return;
    }
    setBusy(true);
    try {
      // Timed around the calls themselves, not around React's work: what is being measured is the library, and
      // including a render would report the page's speed under the library's name.
      const startedAt = globalThis.performance.now();
      const traces = ACTS.map((act) =>
        parseTrace(
          engine.runTrace({
            seed: settings.seed,
            messages: settings.messages,
            faults: settings.faults,
            lines: act.lines,
            glimpse: act.glimpse,
            staleness: act.staleness,
          }),
        ),
      );
      const elapsedMs = globalThis.performance.now() - startedAt;
      setLive((previous) =>
        fastest(previous, { elapsedMs, messages: settings.messages * ACTS.length, runs: 1 }),
      );
      setFilm(buildFilm(traces));
      setError(undefined);
      onRegenerated();
    } catch (cause) {
      setError(cause instanceof TraceFormatError ? cause.message : String(cause));
    } finally {
      setBusy(false);
    }
    // `onRegenerated` deliberately left out of the dependency list: it is a fresh closure every render, and
    // this effect must fire only when `engine` or `settings` actually changes.
  }, [engine, settings]);

  return { engine, film, error, busy, live };
}

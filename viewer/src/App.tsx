// The viewer: press play and watch a market-data feed be damaged and repaired, three times, each time with
// one fewer defence.
//
// There is deliberately nothing to choose. The three runs are one film that plays through, because offering
// them as options(a dropdown, then buttons) made them read as three competing methods rather than three
// layers of one system. See src/model/film.ts.
//
// It reads trace files and draws them. No server, no live connection, and no domain logic: every number on
// screen is a field a trace already carries. See viewer/README.md for why that rule is not tidiness.

import { useCallback, useEffect, useMemo, useState } from "react";
import { parseTrace, TraceFormatError, type Trace } from "./model/trace";
import { PROJECT_ABBREVIATION, PROJECT_NAME, PROJECT_TAGLINE } from "./model/brand";
import {
  ACTS,
  actAt,
  buildFilm,
  DEFAULT_SETTINGS,
  verdictOf,
  dwellFor,
  INTERLUDE_BEATS,
  type Film,
  type Settings,
} from "./model/film";
import { fastest, ratePerSecond, type LiveRun } from "./model/here";
import {
  loadEngine,
  type Engine,
  type SessionParameters,
  type SnapshotParameters,
} from "./wasm/engine";
import { Controls } from "./panels/Controls";
import { parseSession, type SessionTrace } from "./model/session";
import { parseSnapshot, type SnapshotTrace } from "./model/snapshot";
import { SnapshotSection } from "./snapshot/SnapshotSection";
import { SessionSection } from "./session/SessionSection";
import { parseBenchmarks, parseHandoff, type Performance as PerfData } from "./model/perf";
import { Performance } from "./panels/Performance";
import { Disclosure } from "./ui/Disclosure";
import { Hero } from "./panels/Hero";
import { Findings } from "./panels/Findings";
import { Primer } from "./panels/Primer";
import { Outcome } from "./panels/Outcome";
import { plainly } from "./model/plain";
import { nanos } from "./model/perf";
import { TEST_COUNT } from "./model/findings";
import { usePlayback } from "./anim/usePlayback";
import { Sheet } from "./stage/Sheet";
import { Stage } from "./stage/Stage";
import { Transport } from "./panels/Transport";
import { ActCard } from "./panels/ActCard";
import { Prologue } from "./panels/Prologue";
import { ActStrip } from "./panels/ActStrip";
import { Interlude } from "./panels/Interlude";
import { EventRail } from "./panels/EventRail";
import { Summary } from "./panels/Summary";
import { Ledger } from "./panels/Ledger";
import { LineHealth } from "./panels/LineHealth";

export function App() {
  const [film, setFilm] = useState<Film | undefined>();
  const [session, setSession] = useState<SessionTrace | undefined>();
  const [snapshot, setSnapshot] = useState<SnapshotTrace | undefined>();
  const [engine, setEngine] = useState<Engine | undefined>();
  const [settings, setSettings] = useState<Settings>(DEFAULT_SETTINGS);
  const [busy, setBusy] = useState(false);
  const [perf, setPerf] = useState<PerfData | undefined>();
  const [sessionSettings, setSessionSettings] = useState<SessionParameters>({
    orders: 3,
    fill: 40,
    cancel: true,
  });
  // 4,096 is where the committed fixture resumes, so the page opens on the same run the repository ships.
  const [snapshotSettings, setSnapshotSettings] = useState<SnapshotParameters>({
    levels: 5,
    resumeFrom: 4096,
  });
  const [error, setError] = useState<string | undefined>();
  const [started, setStarted] = useState(false);
  // The one measurement on the page the reader causes. See model/here.ts for what it is and is not.
  const [live, setLive] = useState<LiveRun | undefined>(undefined);

  // Pacing belongs to the story, not to the clock: quiet stretches go past, notable moments are held.
  const dwell = useCallback((at: number) => (film === undefined ? 1 : dwellFor(film, at)), [film]);
  const playback = usePlayback(film?.moments.length ?? 0, dwell);

  // The library itself, compiled to WebAssembly. Loaded once, and the page falls back to the committed
  // traces if it cannot be: a reader on a browser without WebAssembly should see the argument, not an
  // error, and the controls say plainly that they are inert rather than pretending to work.
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
      setStarted(false);
    } catch (cause) {
      setError(cause instanceof TraceFormatError ? cause.message : String(cause));
    } finally {
      setBusy(false);
    }
  }, [engine, settings]);

  // The order-entry session, fetched separately and drawn below. Its absence must not stop the film: two
  // halves of one page, and one failing to load is a reason to show the other rather than neither.
  useEffect(() => {
    let cancelled = false;
    if (engine !== undefined) {
      try {
        setSession(parseSession(engine.runSession(sessionSettings)));
      } catch {
        setSession(undefined);
      }
      return;
    }
    fetch("traces/order-session.jsonl")
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error(String(r.status)))))
      .then((text) => {
        if (!cancelled) {
          setSession(parseSession(text));
        }
      })
      .catch(() => {
        if (!cancelled) {
          setSession(undefined);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [engine, sessionSettings]);

  // The snapshot, same arrangement: computed when the library is here, fetched when it is not.
  useEffect(() => {
    let cancelled = false;
    if (engine !== undefined) {
      try {
        setSnapshot(parseSnapshot(engine.runSnapshot(snapshotSettings)));
      } catch {
        setSnapshot(undefined);
      }
      return;
    }
    fetch("traces/glimpse-snapshot.jsonl")
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error(String(r.status)))))
      .then((text) => {
        if (!cancelled) {
          setSnapshot(parseSnapshot(text));
        }
      })
      .catch(() => {
        if (!cancelled) {
          setSnapshot(undefined);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [engine, snapshotSettings]);

  // The benchmark figures. Fetched rather than computed, and the panel says why: WebAssembly cannot time a
  // two-nanosecond operation, so these are native measurements read from a committed file.
  useEffect(() => {
    let cancelled = false;
    const load = async (name: string) => {
      const response = await fetch(`bench/${name}`);
      if (!response.ok) {
        throw new Error(`bench/${name}: ${response.status}`);
      }
      return response.text();
    };
    Promise.all([load("results.json"), load("results-paranoid.json"), load("handoff.json")])
      .then(([shipping, paranoid, handoff]) => {
        if (!cancelled) {
          setPerf({
            shipping: parseBenchmarks(shipping, "the shipping benchmarks"),
            paranoid: parseBenchmarks(paranoid, "the paranoid benchmarks"),
            handoff: parseHandoff(handoff, "the hand-off benchmarks"),
          });
        }
      })
      .catch(() => {
        if (!cancelled) {
          setPerf(undefined);
        }
      });
    return () => {
      cancelled = true;
    };
  }, []);

  // Space to play, arrows to step: the shortcuts anybody tries on something that moves.
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

  const start = useCallback(() => {
    setStarted(true);
    playback.restart();
    playback.play();
  }, [playback]);

  const view = useMemo(() => {
    if (film === undefined) {
      return undefined;
    }
    const at = Math.max(0, Math.min(film.moments.length - 1, Math.floor(playback.position)));
    const moment = film.moments[at];
    if (moment === undefined) {
      return undefined;
    }
    const act = actAt(film, at);
    // Trailing glyphs come from this act only, so nothing from act one is drawn during act two.
    const trail = film.moments
      .slice(Math.max(act.from, at - 3), at)
      .filter((m) => m.beat.fate === "arrive")
      .map((m) => m.beat);
    const messages = film.moments
      .slice(act.from, act.to)
      .reduce((most, m) => Math.max(most, m.beat.event.delivered_before, m.beat.event.end), 1);
    return { at, moment, act, trail, messages };
  }, [film, playback.position]);

  return (
    <div className="app">
      {/* One line, because the page has to say what it is before it says anything else. Everything that used to
          be here(two taglines, a note, a promise about what there is nothing to choose) was competing with
          the drawing for the first thing a reader looks at. */}
      <header className="app__bar">
        <h1 className="app__title">
          {PROJECT_NAME}
          <span className="app__abbr mono">{PROJECT_ABBREVIATION}</span>
        </h1>
        <p className="app__tagline">{PROJECT_TAGLINE}</p>
      </header>

      {error !== undefined && (
        <section className="panel app__error">
          <h2>Could not read the runs</h2>
          <p className="why">{error}</p>
        </section>
      )}

      {film !== undefined && view !== undefined && (
        <main className="app__main">
          {/* Before anything technical. Everything below is downstream of these three sentences. */}
          <Primer
            onWatch={() => {
              document.getElementById("watch")?.scrollIntoView({ behavior: "smooth" });
              start();
            }}
          />

          <Hero
            tests={TEST_COUNT}
            allocations={perf?.shipping.allocations_after_init ?? 0}
            perPacket={
              perf === undefined
                ? "n/a"
                : nanos(
                    perf.shipping.measurements.find((m) => m.name.startsWith("ingest a packet"))
                      ?.best_ns ?? 0,
                  )
            }
            live={engine !== undefined}
            onWatch={() => {
              document.getElementById("watch")?.scrollIntoView({ behavior: "smooth" });
              start();
            }}
          />

          {/* ---- 1. the film ------------------------------------------------ */}
          <section className="act-section" id="watch">
            <h2 className="act-section__title">
              <span className="act-section__number mono">1</span>
              What goes wrong, and how would you even notice?
            </h2>
            <p className="act-section__lede">
              Three acts, played straight through. Each one takes away a defence the last one had, so you watch
              the same system fall a layer deeper. Underneath, a separate test asserts the thing that actually
              matters: the order book rebuilt after loss and repair equals the book that lost nothing.
            </p>

            <ActStrip film={film} position={playback.position} onSeek={(to) => playback.seek(to)} />

            <div className="app__stage">
              <Sheet
                title="MARKET-DATA RECOVERY"
                subtitle={`act ${view.act.ordinal} of III · ${view.act.title.toLowerCase()}`}
                figures={[
                  { label: "DELIVERED", value: String(view.act.trace.summary.messages_delivered) },
                  { label: "TWICE", value: String(view.act.trace.summary.messages_delivered_twice) },
                  { label: "ASKED BACK", value: String(view.act.trace.summary.retransmit_requests) },
                  { label: "LOST", value: String(view.act.trace.summary.unfillable_messages) },
                ]}
              >
                <Stage
                  trace={view.act.trace}
                  beat={view.moment.beat}
                  progress={playback.position - view.at}
                  trail={view.trail}
                  messages={view.messages}
                />
              </Sheet>

              {!started && <Prologue film={film} onStart={start} />}
              {started && view.moment.opening && (
                <Interlude act={view.act} through={view.moment.local / INTERLUDE_BEATS} />
              )}
              <ActCard beat={view.moment.beat} />
            </div>

            <Transport
              playback={playback}
              beats={film.moments.length}
              caption={view.moment.beat.caption}
            />

            {/* Under the transport rather than above the drawing: it changes what you are watching, so it
                belongs with the other things that do. */}
            <Controls
              settings={settings}
              onChange={setSettings}
              busy={busy}
              live={engine !== undefined}
              verdict={verdictOf(film)}
            />

            {/* The same run in words, for a reader who cannot use the drawing. Not a simplification: "the price list
                came out identical to the one that never lost anything" is the claim. */}
            <Outcome outcome={plainly(view.act.trace, view.moment.beat.event)} />

            <EventRail film={film} at={view.at} onSeek={(to) => playback.seek(to)} />

            <Disclosure summary="this act in numbers: delivery, line health, and what the run does not claim">
              <div className="app__aside">
                <Summary trace={view.act.trace} />
                <LineHealth trace={view.act.trace} />
                <Ledger trace={view.act.trace} />
              </div>
            </Disclosure>
          </section>

          {/* ---- 2. the snapshot: the third defence, working ------------------ */}
          {snapshot !== undefined && (
            <section className="act-section">
              <h2 className="act-section__title">
                <span className="act-section__number mono">2</span>
                Starting from nothing: rebuilding the whole price list
              </h2>
              <p className="act-section__lede">
                The last defence, doing its job. Above it is a plane the run falls onto; here a client with no state at
                all is handed the venue&rsquo;s book one frame at a time, and ends up holding it exactly.
              </p>
              <SnapshotSection
                trace={snapshot}
                settings={snapshotSettings}
                onChange={setSnapshotSettings}
                live={engine !== undefined}
              />
            </section>
          )}

          {/* ---- 3. orders ------------------------------------------------- */}
          {session !== undefined && (
            <section className="act-section">
              <h2 className="act-section__title">
                <span className="act-section__number mono">3</span>
                The other direction: sending orders in
              </h2>
              <p className="act-section__lede">
                Above is the exchange sending. This is the exchange listening, and answering every order with a
                numbered reply.
              </p>
              <SessionSection
                trace={session}
                settings={sessionSettings}
                onChange={setSessionSettings}
                live={engine !== undefined}
              />
            </section>
          )}

          {/* ---- 4. the defects --------------------------------------------
              First, because this is what an engineer came to find out and it was invisible: 113 passages in the
              commit history describe a defect found, and not one of them appeared on the page outside a fold. */}
          <section className="act-section">
            <h2 className="act-section__title">
              <span className="act-section__number mono">4</span>
              Where it went wrong while I was building it
            </h2>
            <p className="act-section__lede">
              This section is for engineers, and the rest of the page does not depend on it. Real defects from the
              commit history: several of them mine, found rather than avoided, because a list of things that went
              right is a list of things nobody checked.
            </p>
            <Findings />
          </section>

          {/* ---- 5. cost -------------------------------------------------- */}
          {perf !== undefined && (
            <section className="act-section">
              <h2 className="act-section__title">
                <span className="act-section__number mono">5</span>
                What it costs to keep up
              </h2>
              <p className="act-section__lede">
                The recovery path only runs when something has already gone wrong, which is exactly the code
                nobody benchmarks.
              </p>
              <Performance perf={perf} live={ratePerSecond(live)} />
            </section>
          )}
        </main>
      )}
    </div>
  );
}

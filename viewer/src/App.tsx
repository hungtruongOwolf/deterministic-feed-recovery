// The viewer: press play and watch a market-data feed be damaged and repaired, three times, each time with
// one fewer defence.
//
// There is deliberately nothing to choose. The three runs are one film that plays through, because offering
// them as options — a dropdown, then buttons — made them read as three competing methods rather than three
// layers of one system. See src/model/film.ts.
//
// It reads trace files and draws them. No server, no live connection, and no domain logic: every number on
// screen is a field a trace already carries. See viewer/README.md for why that rule is not tidiness.

import { useCallback, useEffect, useMemo, useState } from "react";
import { parseTrace, TraceFormatError, type Trace } from "./model/trace";
import {
  PAGE_TAGLINE,
  PROJECT_ABBREVIATION,
  PROJECT_NAME,
  PROJECT_TAGLINE,
} from "./model/brand";
import { ACTS, actAt, buildFilm, dwellFor, INTERLUDE_BEATS, type Film } from "./model/film";
import { parseSession, type SessionTrace } from "./model/session";
import { SessionSection } from "./session/SessionSection";
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
  const [error, setError] = useState<string | undefined>();
  const [started, setStarted] = useState(false);

  // Pacing belongs to the story, not to the clock: quiet stretches go past, notable moments are held.
  const dwell = useCallback((at: number) => (film === undefined ? 1 : dwellFor(film, at)), [film]);
  const playback = usePlayback(film?.moments.length ?? 0, dwell);

  useEffect(() => {
    let cancelled = false;
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
          setStarted(false);
        }
      })
      .catch((cause: unknown) => {
        if (!cancelled) {
          setFilm(undefined);
          setError(cause instanceof TraceFormatError ? cause.message : String(cause));
        }
      });
    return () => {
      cancelled = true;
    };
  }, []);

  // The order-entry session, fetched separately and drawn below. Its absence must not stop the film: two
  // halves of one page, and one failing to load is a reason to show the other rather than neither.
  useEffect(() => {
    let cancelled = false;
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
      .reduce((most, m) => Math.max(most, m.beat.event.delivered_through, m.beat.event.end), 1);
    return { at, moment, act, trail, messages };
  }, [film, playback.position]);

  return (
    <div className="app">
      <header className="app__bar">
        <div className="app__brand">
          <h1 className="app__title">
            {PROJECT_NAME}
            <span className="app__abbr mono">{PROJECT_ABBREVIATION}</span>
          </h1>
          <p className="app__tagline">
            {PROJECT_TAGLINE} <span className="app__tagline-sub">{PAGE_TAGLINE}</span>
          </p>
        </div>
        <div className="spacer" />
        <span className="mono app__note">three acts · one system · nothing to choose</span>
      </header>

      {error !== undefined && (
        <section className="panel app__error">
          <h2>Could not read the traces</h2>
          <p className="why">
            {error} — the parser is strict on purpose: a viewer that skipped lines it did not understand
            would draw an incomplete run and look like a complete one.
          </p>
        </section>
      )}

      {film !== undefined && view !== undefined && (
        <main className="app__main">
          <div className="app__stage">
            <ActStrip film={film} position={playback.position} onSeek={(to) => playback.seek(to)} />

            <Sheet
              title="MARKET-DATA RECOVERY · ONE SYSTEM, THREE TIMES"
              subtitle={`act ${view.act.ordinal} of III · ${view.act.title.toLowerCase()} · seed ${view.act.trace.header.seed}`}
              figures={[
                { label: "DELIVERED ONCE", value: String(view.act.trace.summary.messages_delivered) },
                {
                  label: "DELIVERED TWICE",
                  value: String(view.act.trace.summary.messages_delivered_twice),
                },
                { label: "ASKED FOR BACK", value: String(view.act.trace.summary.retransmit_requests) },
                { label: "LOST FOR GOOD", value: String(view.act.trace.summary.unfillable_messages) },
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

          <EventRail film={film} at={view.at} onSeek={(to) => playback.seek(to)} />

          <div className="app__aside">
            <Summary trace={view.act.trace} />
            <LineHealth trace={view.act.trace} />
            <Ledger trace={view.act.trace} />
          </div>

          {session !== undefined && <SessionSection trace={session} />}
        </main>
      )}
    </div>
  );
}

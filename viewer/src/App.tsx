// The viewer: press play and watch a market-data feed be damaged and repaired.
//
// It reads a trace file and draws it. No server, no live connection, and no domain logic — every number on
// screen is a field the trace already carries. See viewer/README.md for why that rule is not tidiness.

import { useCallback, useEffect, useMemo, useState } from "react";
import { parseTrace, TraceFormatError, type Trace } from "./model/trace";
import { buildStory } from "./model/story";
import { usePlayback } from "./anim/usePlayback";
import { Sheet } from "./stage/Sheet";
import { Stage } from "./stage/Stage";
import { Transport } from "./panels/Transport";
import { ActCard } from "./panels/ActCard";
import { Overture } from "./panels/Overture";
import { Summary } from "./panels/Summary";
import { Ledger } from "./panels/Ledger";
import { LineHealth } from "./panels/LineHealth";

const BUNDLED = [
  { file: "redundant-ab.jsonl", label: "① two lines — nobody has to ask for anything" },
  { file: "recovering-seed4711.jsonl", label: "② second line removed — ask for it back" },
  { file: "glimpse-race.jsonl", label: "③ retransmit gone too — the snapshot is too old" },
] as const;

const BUNDLED_NOTE: Record<string, string> = {
  "redundant-ab.jsonl": "defence 1 in place",
  "recovering-seed4711.jsonl": "defence 1 removed",
  "glimpse-race.jsonl": "defences 1 and 2 removed",
};

export function App() {
  const [choice, setChoice] = useState<string>(BUNDLED[0].file);
  const [trace, setTrace] = useState<Trace | undefined>();
  const [error, setError] = useState<string | undefined>();
  const [started, setStarted] = useState(false);

  const story = useMemo(() => (trace === undefined ? [] : buildStory(trace)), [trace]);
  const playback = usePlayback(story.length);

  const load = useCallback((text: string) => {
    try {
      setTrace(parseTrace(text));
      setError(undefined);
      setStarted(false);
    } catch (cause) {
      setTrace(undefined);
      setError(cause instanceof TraceFormatError ? cause.message : String(cause));
    }
  }, []);

  useEffect(() => {
    let cancelled = false;
    fetch(`traces/${choice}`)
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error(String(r.status)))))
      .then((text) => {
        if (!cancelled) {
          load(text);
        }
      })
      .catch((cause: unknown) => {
        if (!cancelled) {
          setError(`could not fetch traces/${choice}: ${String(cause)}`);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [choice, load]);

  // Space to play, arrows to step: the shortcuts anybody tries on something that moves.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) {
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

  const at = Math.min(story.length - 1, Math.floor(playback.position));
  const beat = at >= 0 ? story[at] : undefined;
  const progress = playback.position - at;
  const trail = useMemo(
    () => story.slice(Math.max(0, at - 3), at).filter((b) => b.fate === "arrive"),
    [story, at],
  );
  const messages = useMemo(
    () => story.reduce((most, b) => Math.max(most, b.event.delivered_through, b.event.end), 1),
    [story],
  );

  const start = useCallback(() => {
    setStarted(true);
    playback.restart();
    playback.play();
  }, [playback]);

  return (
    <div className="app">
      <header className="app__bar">
        <h1 className="app__title">
          deterministic feed recovery
          <small>what a market-data client does when the feed breaks</small>
        </h1>
        <div className="spacer" />
        <select value={choice} onChange={(e) => setChoice(e.currentTarget.value)}>
          {BUNDLED.map((option) => (
            <option key={option.file} value={option.file}>
              {option.label}
            </option>
          ))}
        </select>
        <label className="mono app__open">
          open a trace…
          <input
            type="file"
            accept=".jsonl,.json,.txt"
            onChange={(e) => {
              const file = e.currentTarget.files?.[0];
              if (file !== undefined) {
                void file.text().then(load);
              }
            }}
          />
        </label>
      </header>

      {error !== undefined && (
        <section className="panel app__error">
          <h2>Could not read that trace</h2>
          <p className="why">
            {error} — the parser is strict on purpose: a viewer that skipped lines it did not understand
            would draw an incomplete run and look like a complete one.
          </p>
        </section>
      )}

      {trace !== undefined && (
        <main className="app__main">
          <div className="app__stage">
            <Sheet
              title="MARKET-DATA RECOVERY · ONE CONTROLLED RUN"
              subtitle={`seed ${trace.header.seed} · ${BUNDLED_NOTE[choice] ?? trace.header.mode}`}
              figures={[
                { label: "DELIVERED ONCE", value: String(trace.summary.messages_delivered) },
                { label: "DELIVERED TWICE", value: String(trace.summary.messages_delivered_twice) },
                { label: "ASKED FOR BACK", value: String(trace.summary.retransmit_requests) },
                { label: "LOST FOR GOOD", value: String(trace.summary.unfillable_messages) },
              ]}
            >
              <Stage
                trace={trace}
                beat={beat}
                progress={progress}
                trail={trail}
                messages={messages}
              />
            </Sheet>

            {!started && <Overture trace={trace} onStart={start} />}
            <ActCard beat={beat} />
          </div>

          <Transport playback={playback} beats={story.length} caption={beat?.caption ?? ""} />

          <div className="app__aside">
            <Summary trace={trace} />
            <LineHealth trace={trace} />
            <Ledger trace={trace} />
          </div>
        </main>
      )}
    </div>
  );
}

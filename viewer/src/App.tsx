// The viewer: reads a trace file and draws it.
//
// No fetch to a server, no live socket, no state beyond which file is loaded and where the scrubber
// is. That is the point of the trace format existing: the library performs no I/O and reads no
// clock, and a viewer that needed a running process to talk to would have quietly required both.

import { useCallback, useEffect, useMemo, useState } from "react";
import { parseTrace, TraceFormatError, type Trace } from "./model/trace";
import { Scrubber } from "./panels/Scrubber";
import { StateBand } from "./panels/StateBand";
import { GlimpsePanel } from "./panels/GlimpsePanel";
import { LineHealth } from "./panels/LineHealth";
import { Summary } from "./panels/Summary";
import { EventLog } from "./panels/EventLog";
import { Ledger } from "./panels/Ledger";

// The traces committed alongside the code. Each is a deterministic function of its seed, so these
// are fixtures rather than samples: diffing a fresh run against one is a regression test.
const BUNDLED = [
  { file: "recovering-seed4711.jsonl", label: "recovering — one line, faults repaired" },
  { file: "redundant-ab.jsonl", label: "redundant A/B — holes closed by the other line" },
  { file: "glimpse-race.jsonl", label: "glimpse race — the snapshot arrives too old" },
] as const;

export function App() {
  const [choice, setChoice] = useState<string>(BUNDLED[0].file);
  const [trace, setTrace] = useState<Trace | undefined>();
  const [error, setError] = useState<string | undefined>();
  const [index, setIndex] = useState(0);

  const load = useCallback((text: string) => {
    try {
      const parsed = parseTrace(text);
      setTrace(parsed);
      setError(undefined);
      setIndex(0);
    } catch (cause) {
      setTrace(undefined);
      setError(
        cause instanceof TraceFormatError ? cause.message : `could not read the trace: ${String(cause)}`,
      );
    }
  }, []);

  useEffect(() => {
    let cancelled = false;
    fetch(`traces/${choice}`)
      .then((response) => (response.ok ? response.text() : Promise.reject(new Error(String(response.status)))))
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

  const onFile = useCallback(
    (file: File | undefined) => {
      if (file === undefined) {
        return;
      }
      void file.text().then(load);
    },
    [load],
  );

  const header = useMemo(() => trace?.header, [trace]);

  return (
    <div className="app">
      <div className="app__bar">
        <h1 className="app__title">
          deterministic-feed-recovery
          <small>run viewer</small>
        </h1>
        <div className="spacer" />
        {header !== undefined && (
          <span className="mono" style={{ color: "var(--ink-soft)" }}>
            seed {header.seed} · {header.packets} packets · {header.lines === 1 ? "1 line" : "A/B"} ·{" "}
            {header.mode}
          </span>
        )}
        <select value={choice} onChange={(e) => setChoice(e.currentTarget.value)}>
          {BUNDLED.map((option) => (
            <option key={option.file} value={option.file}>
              {option.label}
            </option>
          ))}
        </select>
        <label className="mono" style={{ cursor: "pointer" }}>
          open a trace…
          <input
            type="file"
            accept=".jsonl,.json,.txt"
            style={{ display: "none" }}
            onChange={(e) => onFile(e.currentTarget.files?.[0])}
          />
        </label>
      </div>

      <div className="app__body">
        {error !== undefined && (
          <section className="panel app__wide">
            <h2>Could not read that trace</h2>
            <p className="why" style={{ margin: 0 }}>
              {error} — the parser is strict on purpose: a viewer that skipped lines it did not
              understand would draw an incomplete run and look like a complete one.
            </p>
          </section>
        )}

        {trace !== undefined && (
          <>
            <Scrubber trace={trace} index={index} onChange={setIndex} />
            <StateBand trace={trace} index={index} onChange={setIndex} />
            <div className="stack">
              <GlimpsePanel trace={trace} onChange={setIndex} />
              <LineHealth trace={trace} />
              <Ledger trace={trace} />
            </div>
            <div className="stack">
              <Summary trace={trace} />
              <EventLog trace={trace} index={index} />
            </div>
          </>
        )}
      </div>
    </div>
  );
}

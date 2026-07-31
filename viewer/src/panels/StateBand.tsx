// Every state the run passed through, as one band across the packet axis.
//
// The whole client is five states — synchronising, live, recovering, replaying, failed — and the
// question a reader has is "when did it stop being live, and what happened just before". A band
// answers that in one glance, which a list of events does not.

import type { Trace } from "../model/trace";
import { stateSpans } from "../model/select";

const COLOUR: Record<string, string> = {
  synchronising: "var(--paper-sunk)",
  live: "#dfe8df",
  recovering: "#e2e8f0",
  replaying: "#ece4f0",
  failed: "#f4dcda",
};

interface Props {
  readonly trace: Trace;
  readonly index: number;
  readonly onChange: (index: number) => void;
}

export function StateBand({ trace, index, onChange }: Props) {
  const spans = stateSpans(trace);
  const total = Math.max(1, trace.lastIndex);
  const seen = [...new Set(spans.map((span) => span.state))];

  return (
    <section className="panel app__wide">
      <h2>Client state over the run</h2>
      <p className="why">
        Click a band to jump to where that state began. The marker is the current scrub position.
      </p>
      <div className="band" style={{ position: "relative" }}>
        {spans.map((span, at) => (
          <div
            key={at}
            className="band__span"
            title={`${span.state} — packets ${span.from}..${span.to}`}
            onClick={() => onChange(span.from)}
            style={{
              flexGrow: Math.max(1, span.to - span.from + 1),
              background: COLOUR[span.state] ?? "var(--paper-sunk)",
              borderRight: at === spans.length - 1 ? "none" : "1px solid var(--rule)",
              cursor: "pointer",
            }}
          />
        ))}
        <div
          className="axis__mark"
          style={{ left: `${(index / total) * 100}%`, position: "absolute" }}
        />
      </div>
      <div className="band__legend">
        {seen.map((state) => (
          <span key={state}>
            <span className="band__swatch" style={{ background: COLOUR[state] ?? "var(--paper-sunk)" }} />
            {state}
          </span>
        ))}
      </div>
    </section>
  );
}

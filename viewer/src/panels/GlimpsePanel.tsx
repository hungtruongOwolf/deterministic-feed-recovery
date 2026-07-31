// The Glimpse race, drawn on a sequence axis.
//
// This is the panel the viewer exists for. The failure is arithmetic — three positions on one axis —
// and it is genuinely hard to hold in prose, which is why the commit that implemented it needed
// three paragraphs. Drawn, it takes a second:
//
//   delivered through ─┤        the client handed everything below this downstream
//   snapshot at       ─┤        the state the facility actually froze
//   buffer starts     ─┤        the oldest message the client managed to keep
//
// When the snapshot lands *between* the other two, the messages in that interval are in neither
// place, and no retransmit can help because nothing knows they are missing. A client that did not
// check would apply the snapshot, replay what it holds, and publish a book that is plausible,
// internally consistent, and permanently wrong.

import type { Trace } from "../model/trace";
import { snapshotOutcome } from "../model/select";

interface Props {
  readonly trace: Trace;
  readonly onChange: (index: number) => void;
}

export function GlimpsePanel({ trace, onChange }: Props) {
  const outcome = snapshotOutcome(trace);

  if (outcome === undefined) {
    return (
      <section className="panel">
        <h2>Snapshot recovery</h2>
        <p className="why">
          This run never needed a snapshot: every hole was repaired by retransmission or by the
          redundant line. Load <code>glimpse-race.jsonl</code> to see the race lost.
        </p>
      </section>
    );
  }

  if (!outcome.rejected) {
    return (
      <section className="panel">
        <h2>Snapshot recovery</h2>
        <p className="why">
          The snapshot was taken in time: its position was at or after the oldest buffered message,
          so the buffer replayed on top of it and nothing was lost.
        </p>
        <div className="readout">
          <div className="readout__cell">
            <div className="readout__label">verdict</div>
            <div className="readout__value">
              <span className="tag tag--good">usable</span>
            </div>
          </div>
          <div className="readout__cell">
            <div className="readout__label">replayed</div>
            <div className="readout__value">
              {outcome.unfillableFirst}..{outcome.unfillableEnd}
            </div>
          </div>
        </div>
      </section>
    );
  }

  // Three positions, laid out with a margin either side so the marks are readable.
  const low = outcome.deliveredThrough;
  const snapshot = outcome.unfillableFirst;
  const bufferStart = outcome.unfillableEnd;
  const span = Math.max(1, bufferStart - low);
  const pad = span * 0.18;
  const scale = (value: number) => ((value - low + pad) / (span + pad * 2)) * 100;

  return (
    <section className="panel">
      <h2>Snapshot recovery — the race was lost</h2>
      <p className="why">
        The snapshot froze at {snapshot}. The client had delivered through {low} and the oldest
        message it managed to buffer was {bufferStart}. The {bufferStart - snapshot} messages in
        between are in neither, so no retransmit can help — nothing knows they are missing.
      </p>

      <div className="axis">
        <div
          className="axis__span"
          style={{
            left: `${scale(snapshot)}%`,
            width: `${scale(bufferStart) - scale(snapshot)}%`,
            background: "var(--unfillable)",
          }}
          title={`unfillable: ${snapshot}..${bufferStart}`}
        />
        <div className="axis__mark" style={{ left: `${scale(low)}%` }}>
          <span>delivered {low}</span>
        </div>
        <div className="axis__mark" style={{ left: `${scale(snapshot)}%` }}>
          <span>snapshot {snapshot}</span>
        </div>
        <div className="axis__mark" style={{ left: `${scale(bufferStart)}%` }}>
          <span>buffer {bufferStart}</span>
        </div>
      </div>

      <div className="readout" style={{ marginTop: 14 }}>
        <div className="readout__cell">
          <div className="readout__label">verdict</div>
          <div className="readout__value">
            <span className="tag tag--bad">{outcome.reason}</span>
          </div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">unfillable</div>
          <div className="readout__value">{bufferStart - snapshot}</div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">decided at</div>
          <div className="readout__value">
            <button onClick={() => onChange(outcome.at)}>packet {outcome.at}</button>
          </div>
        </div>
      </div>
    </section>
  );
}

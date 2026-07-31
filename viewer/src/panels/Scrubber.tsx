// The time scrubber: drag through the run one packet at a time.
//
// The x-axis is the packet index, not a timestamp, and that is deliberate. An index is what a seed
// reproduces exactly; a duration is what a machine happened to take. Two people scrubbing the same
// trace see the same thing at the same position.

import type { Trace } from "../model/trace";
import { momentAt } from "../model/select";

interface Props {
  readonly trace: Trace;
  readonly index: number;
  readonly onChange: (index: number) => void;
}

export function Scrubber({ trace, index, onChange }: Props) {
  const moment = momentAt(trace, index);
  const step = (delta: number) =>
    onChange(Math.min(trace.lastIndex, Math.max(0, index + delta)));

  return (
    <section className="panel app__wide">
      <h2>Run position</h2>
      <input
        type="range"
        min={0}
        max={trace.lastIndex}
        value={index}
        onChange={(e) => onChange(Number(e.currentTarget.value))}
        aria-label="packet index"
      />
      <div className="readout" style={{ marginTop: 10 }}>
        <div className="readout__cell">
          <div className="readout__label">packet</div>
          <div className="readout__value">
            {index} / {trace.lastIndex}
          </div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">client state</div>
          <div className="readout__value">{moment.state}</div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">delivered through</div>
          <div className="readout__value">{moment.deliveredThrough}</div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">messages missing</div>
          <div className="readout__value">{moment.missing}</div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">open holes</div>
          <div className="readout__value">{moment.holes}</div>
        </div>
        <div className="spacer" style={{ flex: 1 }} />
        <div className="readout__cell">
          <div className="readout__label">step</div>
          <div>
            <button onClick={() => step(-1)}>&minus;1</button>{" "}
            <button onClick={() => step(1)}>+1</button>{" "}
            <button onClick={() => onChange(trace.lastIndex)}>end</button>
          </div>
        </div>
      </div>
    </section>
  );
}

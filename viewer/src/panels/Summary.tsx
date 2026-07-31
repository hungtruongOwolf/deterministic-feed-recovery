// The run's verdict, and the two numbers that decide it.
//
// "Delivered exactly once" carries as much weight as "nothing missing": a message delivered twice
// because a retransmit crossed a late copy corrupts a book just as thoroughly as one that was
// dropped. Both are shown, and neither is buried.

import type { Trace } from "../model/trace";

interface Props {
  readonly trace: Trace;
}

export function Summary({ trace }: Props) {
  const s = trace.summary;
  const clean = s.messages_missing === 0 && s.messages_delivered_twice === 0;

  return (
    <section className="panel">
      <h2>Verdict</h2>
      <div className="readout">
        <div className="readout__cell">
          <div className="readout__label">delivered once</div>
          <div className="readout__value">{s.messages_delivered}</div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">delivered twice</div>
          <div className="readout__value" style={{ color: s.messages_delivered_twice > 0 ? "var(--unfillable)" : undefined }}>
            {s.messages_delivered_twice}
          </div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">still missing</div>
          <div className="readout__value" style={{ color: s.messages_missing > 0 ? "var(--unfillable)" : undefined }}>
            {s.messages_missing}
          </div>
        </div>
        <div className="readout__cell">
          <div className="readout__label">final state</div>
          <div className="readout__value">{s.final_state}</div>
        </div>
      </div>

      <table style={{ marginTop: 14 }}>
        <tbody>
          <tr>
            <td>retransmit requests</td>
            <td className="num">{s.retransmit_requests}</td>
          </tr>
          <tr>
            <td>served / refused</td>
            <td className="num">
              {s.retransmits_served} / {s.retransmit_refusals}
            </td>
          </tr>
          <tr>
            <td>snapshot requests</td>
            <td className="num">{s.snapshot_requests}</td>
          </tr>
          <tr>
            <td>permanently unfillable</td>
            <td className="num" style={{ color: s.unfillable_messages > 0 ? "var(--unfillable)" : undefined }}>
              {s.unfillable_messages}
            </td>
          </tr>
        </tbody>
      </table>

      {!s.complete && (
        <p className="why" style={{ marginTop: 12, marginBottom: 0, color: "var(--unfillable)" }}>
          The recorder filled: {s.events_dropped} events were not kept, so this trace is a prefix of
          the run. Said plainly rather than drawn as though it were complete.
        </p>
      )}
      {s.complete && (
        <p className="why" style={{ marginTop: 12, marginBottom: 0 }}>
          <span className={clean ? "tag tag--good" : "tag tag--bad"}>
            {clean ? "accounting balances" : "loss recorded"}
          </span>{" "}
          {clean
            ? "every message crossed the boundary exactly once."
            : "the run ended with messages unaccounted for, which is the honest outcome when the venue could not supply them."}
        </p>
      )}
    </section>
  );
}

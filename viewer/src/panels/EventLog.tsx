// The events at the scrub position, and the schedule that caused them.
//
// Two tables rather than one: the schedule is what was *asked for* before the run started, and the
// events are what actually happened. Keeping them apart is the same distinction the library draws
// between a fault a packet was given and a fault it could carry.

import type { Trace, TraceEvent } from "../model/trace";
import { eventsAt } from "../model/select";

const FAULT_EVENTS = new Set([
  "fault_applied",
  "packet_dropped",
  "packet_duplicated",
  "packet_delayed",
  "packet_discarded",
]);
const BAD_EVENTS = new Set(["snapshot_rejected", "range_abandoned", "retransmit_refused"]);

function rowClass(event: TraceEvent): string {
  if (BAD_EVENTS.has(event.event)) {
    return "log--bad";
  }
  return FAULT_EVENTS.has(event.event) ? "log--fault" : "";
}

function range(event: TraceEvent): string {
  return event.end > event.first ? `${event.first}..${event.end}` : "";
}

interface Props {
  readonly trace: Trace;
  readonly index: number;
}

export function EventLog({ trace, index }: Props) {
  const here = eventsAt(trace, index);

  return (
    <section className="panel">
      <h2>At packet {index}</h2>
      <div className="log">
        {here.length === 0 ? (
          <p className="why" style={{ margin: 0 }}>
            Nothing was recorded at this index. The run's state is whatever the last event left it
            as, which the readout above shows.
          </p>
        ) : (
          <table>
            <thead>
              <tr>
                <th>layer</th>
                <th>event</th>
                <th>line</th>
                <th>sequences</th>
                <th>reason</th>
              </tr>
            </thead>
            <tbody>
              {here.map((event, at) => (
                <tr key={at} className={rowClass(event)}>
                  <td>{event.layer}</td>
                  <td className="mono">{event.event}</td>
                  <td className="mono">{event.line === 0 ? "A" : "B"}</td>
                  <td className="mono">{range(event)}</td>
                  <td className="mono">{event.reason === "ok" ? "" : event.reason}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      <h2 style={{ marginTop: 18 }}>Injected schedule</h2>
      <p className="why">
        Decided from the seed before the run began, so it can be printed, diffed and committed —
        which is why a failure here reproduces from a file rather than from a description.
      </p>
      <table>
        <thead>
          <tr>
            <th>op</th>
            <th className="num">from packet</th>
            <th className="num">count</th>
          </tr>
        </thead>
        <tbody>
          {trace.header.schedule.map((fault, at) => (
            <tr key={at}>
              <td className="mono">
                <span className="tag tag--fault">{fault.op}</span>
              </td>
              <td className="num">{fault.first_packet}</td>
              <td className="num">{fault.packet_count}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </section>
  );
}

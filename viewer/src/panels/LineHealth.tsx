// Per-line health.
//
// Losing one line of a redundant pair is invisible in the data: the merged stream stays perfect,
// which is exactly why nothing else notices, and the first anyone hears is when the second line
// fails too. So the numbers worth showing are not error rates — they are how often each line was
// *first*, because a line that never wins a race is either badly delayed or already dead.

import type { Trace } from "../model/trace";
import { lineTallies } from "../model/select";

interface Props {
  readonly trace: Trace;
}

export function LineHealth({ trace }: Props) {
  const tallies = lineTallies(trace);
  const configured = trace.header.lines;

  return (
    <section className="panel">
      <h2>Redundant lines</h2>
      <p className="why">
        {configured > 1
          ? "Both lines carry the same bytes. A line that wins few races is not failing — it is slower. A line that wins none, and is silent, is gone."
          : "This run has a single line, so there is no arbitration to show. Load redundant-ab.jsonl for a pair."}
      </p>
      <table>
        <thead>
          <tr>
            <th>line</th>
            <th className="num">first copy</th>
            <th className="num">duplicate</th>
            <th className="num">discarded</th>
            <th className="num">faults</th>
          </tr>
        </thead>
        <tbody>
          {tallies.map((tally) => (
            <tr key={tally.line}>
              <td>
                <span className="tag">{tally.line === 0 ? "A" : "B"}</span>
              </td>
              <td className="num">{tally.won}</td>
              <td className="num">{tally.duplicate}</td>
              <td className="num">{tally.discarded}</td>
              <td className="num">{tally.faults}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {configured > 1 && trace.summary.retransmit_requests === 0 && (
        <p className="why" style={{ marginTop: 12, marginBottom: 0 }}>
          <span className="tag tag--good">0 retransmit requests</span> — every hole closed from the
          other line before the settle delay expired. That is the entire return on paying for a
          second line, and it is the number that shows it.
        </p>
      )}
    </section>
  );
}

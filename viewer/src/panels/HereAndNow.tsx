// The figure the reader produced, placed before the ones they did not.
//
// It leads the performance section for one reason: it is the only number on the page whose provenance a visitor can
// verify without trusting me. Change the length, press another pattern, and it moves. The committed tables are more
// precise and less believable, and putting the believable one first is what makes the precise ones readable.

import { compact, type LiveRate } from "../model/here";
import { Disclosure } from "../ui/Disclosure";

interface Props {
  readonly rate: LiveRate | undefined;
  readonly nativePerSecond: number | undefined;
}

export function HereAndNow({ rate, nativePerSecond }: Props) {
  if (rate === undefined) {
    return (
      <p className="here here--absent">
        The live figure needs WebAssembly, which did not load — the tables below were measured natively and are
        unaffected.
      </p>
    );
  }

  // How far below native, stated by the page rather than left for a reader to compute and mistrust.
  const factor =
    nativePerSecond === undefined || nativePerSecond <= 0
      ? undefined
      : Math.round(nativePerSecond / rate.messagesPerSecond);

  return (
    <div className="here">
      <div className="here__figure">
        <span className="here__value mono">{compact(rate.messagesPerSecond)}</span>
        <span className="here__unit">messages a second, in your browser</span>
      </div>
      <p className="here__detail">
        {rate.messages.toLocaleString()} messages in <strong>{rate.elapsedMs.toFixed(1)} ms</strong>
        {rate.runs > 1 ? `, fastest of ${rate.runs} runs since you opened this` : ""}. Change anything above and
        it moves.
      </p>
      <Disclosure
        summary={
          factor === undefined
            ? "why this is slower than the tables below"
            : `about ${factor}× slower than the tables below — why`
        }
      >
        <p>
          This path goes through WebAssembly and writes every event out as JSONL as it runs, and the trace format
          is most of what is being timed. A browser also cannot resolve a two-nanosecond operation. So the gap is
          the reason the native tables exist rather than something to hide — but those figures came from my laptop,
          and this one came from yours. It is the fastest run rather than the latest, for the same reason the
          native tables report minima: benchmark noise only ever adds time, and the first run pays for a cold
          instance.
        </p>
      </Disclosure>
    </div>
  );
}

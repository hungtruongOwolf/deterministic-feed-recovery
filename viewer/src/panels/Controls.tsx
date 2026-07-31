// Your run, not mine.
//
// Until this existed the page drew three traces generated once and committed. Everything on it was correct
// and none of it was the reader's: no seed to type, no fault count to raise, nothing to be surprised by. A
// viewer over fixtures is a screenshot with extra steps.
//
// These controls re-run the actual C++ library, compiled to WebAssembly, and the film is rebuilt from what
// it returns. What they change is the *data*. What they cannot change is the argument — which defence each
// act removes is fixed, because that is the experiment, and a control able to break it would turn the page
// back into three unrelated runs.
//
// The verdict below is the honest part, and getting it honest took three attempts. "Each act reaches one
// layer deeper" is false at plenty of seeds. "Two lines mean fewer round trips" is false too, and more
// interestingly — see model/film.ts. What survived a 400-seed sweep is reported here, measured on the run in
// front of the reader rather than asserted about a recording.

import { DEFAULT_SETTINGS, type Settings, type Verdict } from "../model/film";

interface Props {
  readonly settings: Settings;
  readonly onChange: (settings: Settings) => void;
  readonly busy: boolean;
  /** Absent when WebAssembly could not be loaded and the committed traces are being drawn instead. */
  readonly live: boolean;
  /** What this run turned out to be true of. Read from the runs, not from the story. */
  readonly verdict: Verdict;
}

const NUMBERS: ReadonlyArray<{
  readonly key: keyof Settings;
  readonly label: string;
  readonly hint: string;
  readonly min: number;
  readonly max: number;
  readonly step: number;
}> = [
  { key: "seed", label: "seed", hint: "the whole run is a function of this", min: 0, max: 999999, step: 1 },
  { key: "messages", label: "messages", hint: "how long the feed is", min: 40, max: 1200, step: 20 },
  { key: "faults", label: "faults", hint: "how much the injector breaks", min: 0, max: 60, step: 1 },
];

export function Controls({ settings, onChange, busy, live, verdict }: Props) {
  const good = verdict.recoveryHeld && verdict.exactlyOnce;

  return (
    <section className="controls">
      <div className="controls__head mono">
        RUN IT YOURSELF — the C++ library is compiled to WebAssembly and running on this page
      </div>

      {!live && (
        <p className="controls__offline">
          WebAssembly did not load, so the committed traces are being drawn instead. Everything below is
          the run as recorded; the controls are inert rather than pretending.
        </p>
      )}

      <div className="controls__row">
        {NUMBERS.map((field) => (
          <label key={field.key} className="controls__field">
            <span className="controls__label mono">{field.label}</span>
            <input
              className="controls__input mono"
              type="number"
              min={field.min}
              max={field.max}
              step={field.step}
              value={settings[field.key]}
              disabled={!live}
              onChange={(e) => {
                const raw = Number(e.currentTarget.value);
                if (!Number.isFinite(raw)) {
                  return;
                }
                const value = Math.max(field.min, Math.min(field.max, Math.round(raw)));
                onChange({ ...settings, [field.key]: value });
              }}
            />
            <span className="controls__hint">{field.hint}</span>
          </label>
        ))}

        <div className="controls__actions">
          <button
            className="controls__dice"
            disabled={!live || busy}
            onClick={() => onChange({ ...settings, seed: Math.floor(Math.random() * 100000) })}
            title="A seed nobody chose, including me"
          >
            ⚄ &nbsp;another seed
          </button>
          <button
            className="controls__reset"
            disabled={!live || busy}
            onClick={() => onChange(DEFAULT_SETTINGS)}
          >
            back to the committed run
          </button>
        </div>
      </div>

      <div className={`controls__verdict ${good ? "is-holding" : "is-broken"}`}>
        <div className="controls__measures">
          <Measure
            label="LOST FOR GOOD"
            values={verdict.lost}
            ok={verdict.recoveryHeld}
            note="nothing until the last defence answers too late"
          />
          <Measure
            label="DELIVERED TWICE"
            values={verdict.duplicated}
            ok={verdict.exactlyOnce}
            note="never, on any run — a duplicate corrupts a book as surely as a loss"
          />
          <Measure
            label="MESSAGES ASKED BACK"
            values={verdict.roundTripMessages}
            ok
            note="usually far fewer with two lines, though not a law: see below"
          />
        </div>

        <p className="controls__verdict-text">
          {good ? (
            <>
              <strong>Both invariants hold on this run.</strong> Every message arrives exactly once until the
              last defence answers with a moment older than the client needs — and then the client refuses to
              carry on rather than publishing a book it knows to be wrong. Measured here, on your seed, not
              asserted about a recording.
            </>
          ) : (
            <>
              <strong>An invariant does not hold on this run.</strong> Either something was lost before the
              last defence was reached, or something was delivered twice. On a page that measures rather than
              claims, that is worth saying plainly — and worth a bug report if the parameters are ordinary.
            </>
          )}
        </p>

        <p className="controls__aside">
          Two lines usually mean far fewer messages needing a round trip, and it is not a law. Swept over 400
          seeds, two counterexamples: a second line that fills the <em>middle</em> of a hole splits one gap
          into two, so it can produce more requests while asking for fewer messages — and because the
          injector damages each line separately, two lines are not strictly better off either. The invariants
          above held all 400 times; this did not, so it is written as a tendency.
        </p>

        {busy && <span className="controls__busy mono">running…</span>}
      </div>
    </section>
  );
}

function Measure({
  label,
  values,
  ok,
  note,
}: {
  readonly label: string;
  readonly values: readonly number[];
  readonly ok: boolean;
  readonly note: string;
}) {
  return (
    <div className={`measure ${ok ? "is-ok" : "is-bad"}`}>
      <span className="measure__label mono">{label}</span>
      <span className="measure__values mono">{values.join(" · ")}</span>
      <span className="measure__note">{note}</span>
    </div>
  );
}

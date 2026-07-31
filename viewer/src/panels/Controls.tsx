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
// The escalation verdict below is the honest part. It reports the depth each act actually reached rather
// than the depth the story claims, so a reader who sets the fault count to zero sees the claim stop holding
// and is told so, instead of reading a caption that has quietly become false.

import { DEFAULT_SETTINGS, type Settings } from "../model/film";

interface Props {
  readonly settings: Settings;
  readonly onChange: (settings: Settings) => void;
  readonly busy: boolean;
  /** Absent when WebAssembly could not be loaded and the committed traces are being drawn instead. */
  readonly live: boolean;
  /** The depth each act actually reached, in order. Read from the runs, not from the story. */
  readonly depths: readonly number[];
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

export function Controls({ settings, onChange, busy, live, depths }: Props) {
  const holds = depths.length === 3 && depths.every((d, i) => (i === 0 ? true : d > depths[i - 1]!));

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

      <div className={`controls__verdict ${holds ? "is-holding" : "is-broken"}`}>
        <span className="controls__verdict-numbers mono">
          {depths.length === 3 ? depths.join(" → ") : "…"}
        </span>
        <span className="controls__verdict-text">
          {holds ? (
            <>
              <strong>Each act still falls one layer deeper than the last.</strong> That is the claim the
              page makes, measured on this run rather than asserted about a recording.
            </>
          ) : (
            <>
              <strong>On this run the claim does not hold.</strong> With these parameters an act is not
              forced deeper than the one before it — most often because there are too few faults for the
              second act to need a retransmit at all. The page says so rather than keeping a caption that
              has stopped being true.
            </>
          )}
        </span>
        {busy && <span className="controls__busy mono">running…</span>}
      </div>
    </section>
  );
}

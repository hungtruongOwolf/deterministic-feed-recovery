// The controls: two choices, a pattern number, and three figures.
//
// This panel was 399 words. It explained the fault injector, what a seed is, what reproducibility buys, two
// counterexamples from a 400-seed sweep and the reason one claim is a tendency — all of it true, all of it in
// front of somebody who had not yet pressed play.
//
// The material moved rather than went: the sweep and its counterexamples are in the README, and the one
// sentence that has to survive is folded away behind a line. What is left is what a reader needs *in order to
// act*: what they can change, what changed as a result, and whether the run they are looking at held.
//
// The seed is still not asked for. It is the name of a pattern, shown small, because a visitor cannot form an
// intention about a number they have never seen before.

import { DEFAULT_SETTINGS, type Settings, type Verdict } from "../model/film";
import { Disclosure } from "../ui/Disclosure";

interface Props {
  readonly settings: Settings;
  readonly onChange: (settings: Settings) => void;
  readonly busy: boolean;
  readonly live: boolean;
  readonly verdict: Verdict;
}

const DAMAGE = [
  { label: "a little", faults: 2 },
  { label: "typical", faults: 6 },
  { label: "a lot", faults: 24 },
  { label: "brutal", faults: 48 },
] as const;

const LENGTH = [
  { label: "short", messages: 120 },
  { label: "normal", messages: 300 },
  { label: "long", messages: 700 },
] as const;

export function Controls({ settings, onChange, busy, live, verdict }: Props) {
  const good = verdict.recoveryHeld && verdict.exactlyOnce;

  return (
    <div className="controls">
      <div className="controls__row">
        <Choice
          label="damage"
          live={live}
          options={DAMAGE.map((d) => ({
            key: String(d.faults),
            label: d.label,
            on: settings.faults === d.faults,
            choose: () => onChange({ ...settings, faults: d.faults }),
          }))}
        />
        <Choice
          label="length"
          live={live}
          options={LENGTH.map((l) => ({
            key: String(l.messages),
            label: l.label,
            on: settings.messages === l.messages,
            choose: () => onChange({ ...settings, messages: l.messages }),
          }))}
        />

        <label className="controls__pattern">
          <span className="controls__pattern-label mono">pattern</span>
          <input
            className="controls__pattern-input mono"
            type="number"
            min={0}
            max={999999}
            value={settings.seed}
            disabled={!live}
            aria-label="the number naming this pattern of damage"
            onChange={(e) => {
              const raw = Number(e.currentTarget.value);
              if (Number.isFinite(raw)) {
                onChange({ ...settings, seed: Math.max(0, Math.min(999999, Math.round(raw))) });
              }
            }}
          />
        </label>

        <button
          className="controls__dice"
          disabled={!live || busy}
          onClick={() => onChange({ ...settings, seed: Math.floor(Math.random() * 100000) })}
        >
          ⚄ another
        </button>
        <button
          className="controls__reset"
          disabled={!live || busy}
          onClick={() => onChange(DEFAULT_SETTINGS)}
        >
          reset
        </button>

        <div className="controls__figures">
          <Figure label="lost" values={verdict.lost} ok={verdict.recoveryHeld} />
          <Figure label="twice" values={verdict.duplicated} ok={verdict.exactlyOnce} />
          <Figure label="asked back" values={verdict.roundTripMessages} ok />
        </div>

        <span className={`controls__verdict ${good ? "is-holding" : "is-broken"}`}>
          {good ? "both invariants hold" : "an invariant does not hold"}
        </span>
        {busy && <span className="controls__busy mono">running…</span>}
      </div>

      {!live && (
        <p className="controls__offline">
          WebAssembly did not load, so these are the recorded runs and the controls are switched off.
        </p>
      )}

      <Disclosure summary="what these three figures mean, and which of them is a law">
        <p>
          One number per act. <strong>Lost</strong> and <strong>twice</strong> are the invariants: swept over
          400 patterns, nothing was ever delivered twice and nothing was ever lost until the last defence
          answered too late. <strong>Asked back</strong> is a tendency, not a law — two lines usually mean far
          fewer messages needing a round trip, and there are counterexamples. The <em>pattern</em> number names
          this run: type it back in a year and the same packets go missing at the same moments, on any machine.
        </p>
      </Disclosure>
    </div>
  );
}

interface Option {
  readonly key: string;
  readonly label: string;
  readonly on: boolean;
  readonly choose: () => void;
}

function Choice({
  label,
  options,
  live,
}: {
  readonly label: string;
  readonly options: readonly Option[];
  readonly live: boolean;
}) {
  return (
    <div className="choice">
      <span className="choice__label mono">{label}</span>
      <div className="choice__options">
        {options.map((option) => (
          <button
            key={option.key}
            className={`choice__option ${option.on ? "is-on" : ""}`}
            disabled={!live}
            onClick={option.choose}
          >
            {option.label}
          </button>
        ))}
      </div>
    </div>
  );
}

function Figure({
  label,
  values,
  ok,
}: {
  readonly label: string;
  readonly values: readonly number[];
  readonly ok: boolean;
}) {
  return (
    <div className={`measure ${ok ? "is-ok" : "is-bad"}`}>
      <span className="measure__label mono">{label}</span>
      <span className="measure__values mono">{values.join("·")}</span>
    </div>
  );
}

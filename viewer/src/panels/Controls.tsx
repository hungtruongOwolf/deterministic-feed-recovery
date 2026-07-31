// The controls, in the reader's language rather than mine.
//
// The first version of this asked for a **seed**. That is the right word inside the library and the wrong word
// on a page: a visitor does not know what differs between seed 4711 and seed 4712, cannot tell which to pick,
// and has no reason to think either is interesting. Offering somebody a text box they cannot form an intention
// about is worse than offering nothing, because now the page looks interactive and is not.
//
// So the controls say what they mean. "How much goes wrong" is a choice a reader can hold an opinion about;
// "how much of the feed you watch" is another. The seed is still there — it has to be, because reproducibility
// is the point of the whole project — but it is demoted to what it actually is: the name of *this particular
// pattern of damage*, shown so it can be typed back in, not asked for up front.
//
// The verdict below is measured on the run in front of the reader. Getting it honest took three attempts and a
// 400-seed sweep; see model/film.ts for the two claims that turned out to be false.

import { DEFAULT_SETTINGS, type Settings, type Verdict } from "../model/film";

interface Props {
  readonly settings: Settings;
  readonly onChange: (settings: Settings) => void;
  readonly busy: boolean;
  /** False when WebAssembly could not be loaded and the committed runs are being drawn instead. */
  readonly live: boolean;
  readonly verdict: Verdict;
}

// Named levels rather than a number, because "24 faults" is not a quantity anybody has intuition for.
//
// The numbers behind them are chosen from the sweep: at 2 the second act often needs no retransmit at all, at
// 6 the three acts separate cleanly, and at 24 the snapshot path is under real pressure.
const DAMAGE: ReadonlyArray<{ readonly label: string; readonly faults: number; readonly note: string }> = [
  { label: "a little", faults: 2, note: "a few packets lost" },
  { label: "typical", faults: 6, note: "a normal bad morning" },
  { label: "a lot", faults: 24, note: "bursts, duplicates, a line diverging" },
  { label: "brutal", faults: 48, note: "more damage than any real feed sees" },
];

const LENGTH: ReadonlyArray<{ readonly label: string; readonly messages: number }> = [
  { label: "short", messages: 120 },
  { label: "normal", messages: 300 },
  { label: "long", messages: 700 },
];

export function Controls({ settings, onChange, busy, live, verdict }: Props) {
  const good = verdict.recoveryHeld && verdict.exactlyOnce;

  return (
    <section className="controls">
      <div className="controls__head mono">
        THIS IS RUNNING, NOT REPLAYING — the C++ library is compiled to WebAssembly and executing on this page
      </div>

      {!live && (
        <p className="controls__offline">
          WebAssembly did not load, so the recorded runs are being drawn instead. Everything below is real, and
          the controls are switched off rather than pretending to work.
        </p>
      )}

      <div className="controls__row">
        <Choice
          label="how much goes wrong"
          help="the fault injector damages the feed on purpose — this is how hard"
          options={DAMAGE.map((d) => ({
            key: String(d.faults),
            label: d.label,
            note: d.note,
            on: settings.faults === d.faults,
            choose: () => onChange({ ...settings, faults: d.faults }),
          }))}
          live={live}
        />

        <Choice
          label="how much of the feed"
          help="how many messages the venue publishes before the run ends"
          options={LENGTH.map((l) => ({
            key: String(l.messages),
            label: l.label,
            note: `${l.messages} messages`,
            on: settings.messages === l.messages,
            choose: () => onChange({ ...settings, messages: l.messages }),
          }))}
          live={live}
        />

        <div className="controls__actions">
          <button
            className="controls__dice"
            disabled={!live || busy}
            onClick={() => onChange({ ...settings, seed: Math.floor(Math.random() * 100000) })}
            title="A different random pattern of damage. Nobody chose it, including me."
          >
            ⚄ &nbsp;a different pattern
          </button>
          <button
            className="controls__reset"
            disabled={!live || busy}
            onClick={() => onChange(DEFAULT_SETTINGS)}
          >
            back to the recorded one
          </button>
        </div>
      </div>

      {/* The seed, demoted to a footnote: the name of this pattern, not a question to answer. */}
      <div className="controls__pattern">
        <span className="controls__pattern-label mono">PATTERN</span>
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
            if (!Number.isFinite(raw)) {
              return;
            }
            onChange({ ...settings, seed: Math.max(0, Math.min(999999, Math.round(raw))) });
          }}
        />
        <span className="controls__pattern-note">
          The damage is random and <em>reproducible</em>: this number names the pattern. Type it back in a year
          and the same packets go missing at the same moments — on any machine, in any browser. That is what
          "deterministic" is doing in the title, and it is why a bug in this library can be handed over as a
          number rather than a description.
        </span>
      </div>

      <div className={`controls__verdict ${good ? "is-holding" : "is-broken"}`}>
        <div className="controls__measures">
          <Measure
            label="LOST FOR GOOD"
            values={verdict.lost}
            ok={verdict.recoveryHeld}
            note="one figure per act — nothing until the last defence answers too late"
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
              continue rather than publishing a book it knows to be wrong. Measured here, on this pattern, not
              asserted about a recording.
            </>
          ) : (
            <>
              <strong>An invariant does not hold on this run.</strong> Either something was lost before the
              last defence was reached, or something was delivered twice. On a page that measures rather than
              claims, that is worth saying plainly — and worth a bug report if the settings are ordinary.
            </>
          )}
        </p>

        <p className="controls__aside">
          Two lines usually mean far fewer messages needing a round trip, and it is not a law. Swept over 400
          patterns, two counterexamples: a second line that fills the <em>middle</em> of a hole splits one gap
          into two, so it can produce more requests while asking for fewer messages — and because the injector
          damages each line separately, two lines are not strictly better off either. The invariants above held
          all 400 times; this did not, so it is written as a tendency.
        </p>

        {busy && <span className="controls__busy mono">running…</span>}
      </div>
    </section>
  );
}

interface Option {
  readonly key: string;
  readonly label: string;
  readonly note: string;
  readonly on: boolean;
  readonly choose: () => void;
}

function Choice({
  label,
  help,
  options,
  live,
}: {
  readonly label: string;
  readonly help: string;
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
            title={option.note}
          >
            {option.label}
          </button>
        ))}
      </div>
      <span className="choice__help">{help}</span>
    </div>
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

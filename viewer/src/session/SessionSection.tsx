// The second half of the page: orders coming in, where the film above is market data going out.
//
// Always visible, below the film rather than behind a control. The two are the two directions of one
// venue — a counterparty that only published would not be one — so they are two sections of one page, not
// two things to pick between. The page reads top to bottom: what the exchange sends, then what it accepts.

import { useState } from "react";
import { meaningOf, type SessionTrace } from "../model/session";
import type { SessionParameters } from "../wasm/engine";
import { Ladder } from "./Ladder";

interface Props {
  readonly trace: SessionTrace;
  readonly settings: SessionParameters;
  readonly onChange: (settings: SessionParameters) => void;
  /** False when WebAssembly could not load and the committed session is being drawn. */
  readonly live: boolean;
}

export function SessionSection({ trace, settings, onChange, live }: Props) {
  const [hover, setHover] = useState<number | undefined>(undefined);
  const focused = hover === undefined ? undefined : trace.steps[hover];
  const { summary, header } = trace;

  return (
    <section className="session">
      <header className="session__head">
        <h2 className="session__title">The other direction: orders coming in</h2>
        <p className="session__lede">
          The same venue accepts orders over OUCH 4.2 carried on a SoupBinTCP session. It is the half a
          feed publisher is not: a counterparty has to answer as well as broadcast. Below is one real
          session, recorded by <code>tools/session</code> and drawn from the recording.
        </p>
      </header>

      <div className="session__claim">
        <div className="session__claim-text">
          <strong>Watch the two outer columns.</strong> SoupBinTCP puts the sequence number of a packet{" "}
          <em>nowhere in the packet</em>. The exchange assigns it; the client derives it by counting what
          arrives. They are equal on every rung — and the number never crossed between them. That
          agreement is the only evidence either side is right, and it is what the session component exists
          to get correct.
        </div>
        <div className={`session__verdict ${summary.agreed ? "is-agreed" : "is-broken"}`}>
          <span className="session__verdict-pair mono">
            {summary.client_next} = {summary.server_next}
          </span>
          <span className="session__verdict-word">
            {summary.agreed ? "they agree" : "they disagree"}
          </span>
        </div>
      </div>

      <div className="session__controls">
        <span className="session__controls-head mono">YOUR SESSION</span>
        <label className="controls__field controls__field--tight">
          <span className="controls__label mono">orders</span>
          <input
            className="controls__input mono"
            type="number"
            min={1}
            max={12}
            value={settings.orders}
            disabled={!live}
            onChange={(e) =>
              onChange({ ...settings, orders: clamp(Number(e.currentTarget.value), 1, 12) })
            }
          />
        </label>
        <label className="controls__field controls__field--tight">
          <span className="controls__label mono">fill shares</span>
          <input
            className="controls__input mono"
            type="number"
            min={0}
            max={200}
            step={20}
            value={settings.fill}
            disabled={!live}
            onChange={(e) =>
              onChange({ ...settings, fill: clamp(Number(e.currentTarget.value), 0, 200) })
            }
          />
        </label>
        <label className="session__toggle">
          <input
            type="checkbox"
            checked={settings.cancel}
            disabled={!live}
            onChange={(e) => onChange({ ...settings, cancel: e.currentTarget.checked })}
          />
          <span>cancel the second order</span>
        </label>
        <span className="session__controls-note">
          {live
            ? "Fill all 200 shares and watch the order go from live to dead, and the counters keep step."
            : "WebAssembly did not load, so this is the committed session and the controls are inert."}
        </span>
      </div>

      <Ladder trace={trace} hover={hover} onHover={setHover} />

      <div className="session__caption">
        {focused === undefined ? (
          <span className="session__caption-idle">
            Hover a step to read what it means. Solid arrows are the numbered stream; light ones carry no
            position.
          </span>
        ) : (
          <>
            <span className="session__caption-name mono">
              {focused.type} · {focused.name}
            </span>
            <span className="session__caption-body">{meaningOf(focused)}</span>
          </>
        )}
      </div>

      <div className="session__figures">
        <Figure label="ORDERS IN" value={String(summary.orders_in)} />
        <Figure label="ACKS OUT" value={String(summary.acknowledgements_out)} />
        <Figure label="STILL LIVE" value={String(summary.live_orders)} />
        <Figure label="ACCOUNTING" value={summary.accounts ? "balances" : "BROKEN"} />
        <Figure label="ENDED BY" value={summary.ending.replace(/_/g, " ")} />
      </div>

      <p className="session__foot">
        {header.orders} orders, a fill of {header.fill} shares driven by the caller
        {header.cancel ? ", one cancel" : ""}, one logout. There is no matching engine here on purpose — see the ledger for what that means and what
        else this run does not claim.
      </p>
    </section>
  );
}

function clamp(value: number, low: number, high: number): number {
  if (!Number.isFinite(value)) {
    return low;
  }
  return Math.max(low, Math.min(high, Math.round(value)));
}

function Figure({ label, value }: { readonly label: string; readonly value: string }) {
  return (
    <div className="session__figure">
      <span className="session__figure-label mono">{label}</span>
      <span className="session__figure-value">{value}</span>
    </div>
  );
}

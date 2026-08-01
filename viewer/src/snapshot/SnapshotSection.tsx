// The third defence, working.
//
// Everything else on the page shows what happens *because* a snapshot was reached: the escalation marker falls to the
// bottom plane, and in the worst act the book comes out incomplete. Neither shows a snapshot doing its job.
//
// This does, and the controls are live for the same reason the rest of the page's are: a depth a reader chose is a run
// that happened.

import { useState } from "react";
import type { SnapshotTrace } from "../model/snapshot";
import type { SnapshotParameters } from "../wasm/engine";
import { Rebuild } from "./Rebuild";
import { Disclosure } from "../ui/Disclosure";

interface Props {
  readonly trace: SnapshotTrace;
  readonly settings: SnapshotParameters;
  readonly onChange: (settings: SnapshotParameters) => void;
  readonly live: boolean;
}

const DEPTHS = [2, 5, 10, 16] as const;

export function SnapshotSection({ trace, settings, onChange, live }: Props) {
  const [hover, setHover] = useState<number | undefined>(undefined);

  return (
    <>
      <div className="snapshot__controls">
        <span className="snapshot__controls-head mono">depth the venue holds</span>
        <div className="choice__options">
          {DEPTHS.map((levels) => (
            <button
              key={levels}
              className={`choice__option ${settings.levels === levels ? "is-on" : ""}`}
              disabled={!live}
              aria-label={`a book ${levels} levels deep on each side`}
              aria-pressed={settings.levels === levels}
              onClick={() => onChange({ ...settings, levels })}
            >
              {levels}
            </button>
          ))}
        </div>
        <span className="snapshot__controls-note">
          {live
            ? "every level crosses as its own frame, so a deeper book is a longer session and the same protocol"
            : "recorded run; controls off"}
        </span>
      </div>

      <Rebuild trace={trace} hover={hover} onHover={setHover} />

      <Disclosure summary="why a snapshot sends a book rather than replaying the day">
        <p>
          The obvious implementation is &ldquo;resend everything from the open&rdquo;, and it is wrong for the reason
          snapshots exist: a client asks for one because it <em>cannot</em> catch up by replay. So the service walks
          its book and sends one frame per level: O(depth), not O(messages).
        </p>
        <p>
          The consequence is worth knowing rather than hiding: <strong>a snapshot carries state, not history.</strong>
          {" "}A client that applies one holds the right book and has seen none of the trades that built it. Anything
          reconciling volume has to account for that, and a service that replayed trades to disguise it would be
          replaying the day.
        </p>
      </Disclosure>
    </>
  );
}

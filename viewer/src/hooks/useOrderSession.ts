// The order-entry session, computed when the library is here and fetched when it is not.
//
// Two halves of one page: the film above draws the venue sending, this draws the venue listening. One failing
// to load must not stop the other, so a failure here leaves `session` undefined rather than surfacing an error.

import { useEffect, useState } from "react";
import { parseSession, type SessionTrace } from "../model/session";
import { type Engine, type SessionParameters } from "../wasm/engine";

export function useOrderSession(
  engine: Engine | undefined,
  sessionSettings: SessionParameters,
): SessionTrace | undefined {
  const [session, setSession] = useState<SessionTrace | undefined>();

  useEffect(() => {
    let cancelled = false;
    if (engine !== undefined) {
      try {
        setSession(parseSession(engine.runSession(sessionSettings)));
      } catch {
        setSession(undefined);
      }
      return;
    }
    fetch("traces/order-session.jsonl")
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error(String(r.status)))))
      .then((text) => {
        if (!cancelled) {
          setSession(parseSession(text));
        }
      })
      .catch(() => {
        if (!cancelled) {
          setSession(undefined);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [engine, sessionSettings]);

  return session;
}

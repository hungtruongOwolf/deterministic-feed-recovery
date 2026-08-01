// The snapshot rebuild, same arrangement as the order session: computed when the library is here, fetched
// when it is not.

import { useEffect, useState } from "react";
import { parseSnapshot, type SnapshotTrace } from "../model/snapshot";
import { type Engine, type SnapshotParameters } from "../wasm/engine";

export function useGlimpseSnapshot(
  engine: Engine | undefined,
  snapshotSettings: SnapshotParameters,
): SnapshotTrace | undefined {
  const [snapshot, setSnapshot] = useState<SnapshotTrace | undefined>();

  useEffect(() => {
    let cancelled = false;
    if (engine !== undefined) {
      try {
        setSnapshot(parseSnapshot(engine.runSnapshot(snapshotSettings)));
      } catch {
        setSnapshot(undefined);
      }
      return;
    }
    fetch("traces/glimpse-snapshot.jsonl")
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error(String(r.status)))))
      .then((text) => {
        if (!cancelled) {
          setSnapshot(parseSnapshot(text));
        }
      })
      .catch(() => {
        if (!cancelled) {
          setSnapshot(undefined);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [engine, snapshotSettings]);

  return snapshot;
}

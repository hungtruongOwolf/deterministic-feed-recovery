// The benchmark figures. Fetched rather than computed, and the panel says why: WebAssembly cannot time a
// two-nanosecond operation, so these are native measurements read from a committed file.

import { useEffect, useState } from "react";
import { parseBenchmarks, parseHandoff, type Performance as PerfData } from "../model/perf";

export function usePerformanceData(): PerfData | undefined {
  const [perf, setPerf] = useState<PerfData | undefined>();

  useEffect(() => {
    let cancelled = false;
    const load = async (name: string) => {
      const response = await fetch(`bench/${name}`);
      if (!response.ok) {
        throw new Error(`bench/${name}: ${response.status}`);
      }
      return response.text();
    };
    Promise.all([load("results.json"), load("results-paranoid.json"), load("handoff.json")])
      .then(([shipping, paranoid, handoff]) => {
        if (!cancelled) {
          setPerf({
            shipping: parseBenchmarks(shipping, "the shipping benchmarks"),
            paranoid: parseBenchmarks(paranoid, "the paranoid benchmarks"),
            handoff: parseHandoff(handoff, "the hand-off benchmarks"),
          });
        }
      })
      .catch(() => {
        if (!cancelled) {
          setPerf(undefined);
        }
      });
    return () => {
      cancelled = true;
    };
  }, []);

  return perf;
}

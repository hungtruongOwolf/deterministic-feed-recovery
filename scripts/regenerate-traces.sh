#!/usr/bin/env bash
# Regenerates the committed traces, and the viewer's copies of them.
#
# The traces in traces/ are fixtures rather than samples: each is a deterministic function of its
# seed, so `git diff` after running this is a behavioural regression report. An empty diff means
# nothing observable changed; a non-empty one is worth reading line by line.
set -euo pipefail

preset="${1:-dev-local}"
trace="build/${preset}/tools/trace"

if [[ ! -x "${trace}" ]]; then
  echo "regenerate-traces: ${trace} not built; run cmake --build --preset ${preset}" >&2
  exit 1
fi

"${trace}" --seed 4711 --messages 300 --out traces/recovering-seed4711.jsonl
"${trace}" --seed 4711 --messages 300 --lines 2 --out traces/redundant-ab.jsonl
"${trace}" --glimpse --messages 300 --out traces/glimpse-race.jsonl

# The viewer serves them from its own public/ so that a static build carries its fixtures with it.
cp traces/*.jsonl viewer/public/traces/
echo "regenerate-traces: done; git diff traces/ viewer/public/traces/ is the regression report"

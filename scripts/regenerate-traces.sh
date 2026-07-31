#!/usr/bin/env bash
# Regenerates the committed traces, and the viewer's copies of them.
#
# The traces in traces/ are fixtures rather than samples: each is a deterministic function of its
# seed, so `git diff` after running this is a behavioural regression report. An empty diff means
# nothing observable changed; a non-empty one is worth reading line by line.
set -euo pipefail

preset="${1:-dev-local}"
trace="build/${preset}/tools/trace"
session="build/${preset}/tools/session"
glimpse="build/${preset}/tools/glimpse"

for tool in "${trace}" "${session}" "${glimpse}"; do
  if [[ ! -x "${tool}" ]]; then
    echo "regenerate-traces: ${tool} not built; run cmake --build --preset ${preset}" >&2
    exit 1
  fi
done

"${trace}" --seed 4711 --messages 300 --out traces/recovering-seed4711.jsonl
"${trace}" --seed 4711 --messages 300 --lines 2 --out traces/redundant-ab.jsonl
"${trace}" --glimpse --messages 300 --out traces/glimpse-race.jsonl

# The order-entry session, which the viewer draws below the film. Also a fixture: no clock is read and no
# randomness is involved, so a diff here is a change in the session's behaviour and nothing else.
"${session}" --quiet --trace traces/order-session.jsonl

# The snapshot session, which the viewer draws as the third defence actually working. Also a fixture: no clock, no
# randomness, so a diff here is a change in the service or the client and nothing else.
"${glimpse}" --quiet --levels 5 --resume 4096 --trace traces/glimpse-snapshot.jsonl

# The viewer serves them from its own public/ so that a static build carries its fixtures with it.
cp traces/*.jsonl viewer/public/traces/
echo "regenerate-traces: done; git diff traces/ viewer/public/traces/ is the regression report"

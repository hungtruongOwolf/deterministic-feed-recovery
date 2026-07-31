#!/usr/bin/env bash
# Does the browser produce the same trace as the terminal?
#
# This is the load-bearing check for the WebAssembly build, and it is the reason the page can claim to be
# running the library rather than a port of it. Two compilers, two targets, one seed: if the bytes differ
# then something in the library depends on the platform, and "deterministic" was a word rather than a
# property.
#
# It also catches the thing a size comparison would miss. A trace whose numbers are right and whose
# formatting differs by a space is still a second format, and the viewer would have to tolerate both.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-dev}"
native="${here}/build/${preset}/tools/trace"
native_session="${here}/build/${preset}/tools/session"
module="${here}/viewer/public/wasm/dfr.js"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

for needed in "${native}" "${native_session}" "${module}"; do
  if [[ ! -f "${needed}" ]]; then
    echo "check-wasm: ${needed} missing; build the tools and run scripts/build-wasm.sh" >&2
    exit 1
  fi
done

# The runs to compare. Deliberately not just the defaults: a platform difference could easily live in the
# glimpse path, or only appear once two lines are arbitrating, so each shape gets asked.
cat > "${work}/run.mjs" <<'EOF'
import createDfr from "../viewer/public/wasm/dfr.js";
import { writeFileSync } from "node:fs";

const dfr = await createDfr();
const trace = dfr.cwrap("dfr_run_trace", "string", ["number", "number", "number", "number", "number", "number"]);
const session = dfr.cwrap("dfr_run_session", "string", ["number", "number", "number"]);

const cases = JSON.parse(process.argv[2]);
for (const c of cases) {
  const text = c.kind === "session"
    ? session(c.orders, c.fill, c.cancel)
    : trace(c.seed, c.messages, c.faults, c.lines, c.glimpse, c.staleness);
  writeFileSync(c.out, text);
}
EOF
cp "${work}/run.mjs" "${here}/scripts/.check-wasm-run.mjs"
trap 'rm -rf "${work}"; rm -f "${here}/scripts/.check-wasm-run.mjs"' EXIT

cases='[
  {"kind":"trace","seed":4711,"messages":300,"faults":6,"lines":1,"glimpse":0,"staleness":0,"out":"OUT/a.wasm.jsonl"},
  {"kind":"trace","seed":4711,"messages":300,"faults":6,"lines":2,"glimpse":0,"staleness":0,"out":"OUT/b.wasm.jsonl"},
  {"kind":"trace","seed":9001,"messages":700,"faults":24,"lines":1,"glimpse":0,"staleness":0,"out":"OUT/c.wasm.jsonl"},
  {"kind":"trace","seed":4711,"messages":300,"faults":6,"lines":1,"glimpse":1,"staleness":20,"out":"OUT/d.wasm.jsonl"},
  {"kind":"session","orders":3,"fill":40,"cancel":1,"out":"OUT/e.wasm.jsonl"},
  {"kind":"session","orders":8,"fill":200,"cancel":0,"out":"OUT/f.wasm.jsonl"}
]'
cases="${cases//OUT/${work}}"

node "${here}/scripts/.check-wasm-run.mjs" "${cases}"

"${native}" --seed 4711 --messages 300 --faults 6            --out "${work}/a.native.jsonl" >/dev/null
"${native}" --seed 4711 --messages 300 --faults 6 --lines 2   --out "${work}/b.native.jsonl" >/dev/null
"${native}" --seed 9001 --messages 700 --faults 24            --out "${work}/c.native.jsonl" >/dev/null
"${native}" --glimpse --seed 4711 --messages 300 --faults 6 --staleness 20 --out "${work}/d.native.jsonl" >/dev/null
"${native_session}" --quiet --orders 3 --fill 40             --trace "${work}/e.native.jsonl"
"${native_session}" --quiet --orders 8 --fill 200 --no-cancel --trace "${work}/f.native.jsonl"

failures=0
for name in a b c d e f; do
  if diff -q "${work}/${name}.native.jsonl" "${work}/${name}.wasm.jsonl" >/dev/null; then
    lines="$(wc -l < "${work}/${name}.wasm.jsonl" | tr -d ' ')"
    echo "  ✓ ${name}: identical (${lines} lines)"
  else
    echo "  ✗ ${name}: the browser and the terminal disagree"
    diff "${work}/${name}.native.jsonl" "${work}/${name}.wasm.jsonl" | head -8
    failures=$((failures + 1))
  fi
done

if [[ ${failures} -ne 0 ]]; then
  echo "check-wasm: ${failures} of 6 runs differ between native and WebAssembly" >&2
  exit 1
fi
echo "check-wasm: the browser and the terminal produce the same bytes for all 6 runs"

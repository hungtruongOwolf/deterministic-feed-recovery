#!/usr/bin/env bash
# Builds the benchmark at one optimisation level and three assertion levels, and writes the JSON.
#
# Three builds rather than reusing the dev/release/bench presets, on purpose. Those presets differ in *both*
# optimisation level and assertion level, so comparing them prices two variables at once and reports the sum
# as though it were the cost of the assertions — which gave a 55x ratio that was a real number about nothing
# anybody would ship. See docs/BENCHMARKS.md.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
samples="${1:-400}"

for level in paranoid fast off; do
  dir="${here}/build/o3-${level}"
  cmake -S "${here}" -B "${dir}" \
    -DCMAKE_BUILD_TYPE=Release -DDFR_ASSERTIONS="${level}" -DDFR_WARNINGS_AS_ERRORS=ON >/dev/null
  cmake --build "${dir}" --target dfr_recovery_bench -j 8 >/dev/null
  echo "── assertions=${level} ─────────────────────────────────────────────"
  "${dir}/bench/recovery_bench" --samples "${samples}" \
    --json "${here}/bench/results-${level}.json"
done

# The viewer draws the shipping configuration. The other two are committed next to it so the assertion cost
# is a diff anybody can read rather than a sentence in a document.
cp "${here}/bench/results-off.json" "${here}/viewer/public/bench/results.json"
cp "${here}/bench/results-paranoid.json" "${here}/viewer/public/bench/results-paranoid.json"
echo "run-benchmarks: wrote bench/results-*.json and the viewer's copies"

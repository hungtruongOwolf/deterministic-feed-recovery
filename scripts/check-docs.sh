#!/usr/bin/env bash
# Do the numbers in the documentation still match the build?
#
# A README that says 676 tests when there are 715 is the worst kind of defect in a portfolio: it is the first
# thing a reader checks and the easiest thing to leave behind. `sync-test-count.sh` already keeps the *page*
# honest; this keeps the prose honest, and CI runs it so staleness fails a build instead of ageing quietly.
#
# It checks numbers and preset names, not wording. A grep that enforced prose would be a grep somebody works
# around by rephrasing.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-dev}"
failures=0

say() { printf '  %s %s\n' "$1" "$2"; }

count="$(ctest --test-dir "${here}/build/${preset}" -N | sed -n 's/^Total Tests: //p')"
if [[ -z "${count}" ]]; then
  echo "check-docs: could not read a test count from ctest" >&2
  exit 1
fi

# Every test count written down anywhere has to be the real one.
stale="$(grep -rnoE '\b[0-9]{3}\b tests' "${here}/README.md" "${here}/docs" "${here}/viewer/README.md" 2>/dev/null \
  | grep -v "${count} tests" || true)"
if [[ -n "${stale}" ]]; then
  say "✗" "a stale test count is written down:"
  printf '      %s\n' "${stale}"
  failures=$((failures + 1))
else
  say "✓" "every test count in the docs says ${count}"
fi

# The preset list is the other number that ages. Five configurations plus a third compiler.
presets="$(ls "${here}/CMakePresets.json" >/dev/null && python3 -c "
import json
print(len([p for p in json.load(open('${here}/CMakePresets.json'))['configurePresets'] if p['name'] != 'base']))")"
if grep -qE 'all four of|same four presets' "${here}/README.md"; then
  say "✗" "the README still describes four presets; there are ${presets}"
  failures=$((failures + 1))
else
  say "✓" "the README does not undercount the ${presets} presets"
fi

# Every wire protocol gets an umbrella header, and only wire protocols do.
#
# I got this wrong first: I assumed every namespace had one and added two that did not belong. It is a convention
# about *protocols* — the thing an external consumer includes by name, "give me SoupBinTCP" — and `dfr::book` with
# one header does not need a file whose only content is including that header. Enforcing the convention that
# exists beats enforcing the one I imagined.
missing=0
for dir in "${here}"/include/dfr/wire/*/; do
  name="$(basename "${dir}")"
  if [[ ! -f "${here}/include/dfr/wire/${name}.hpp" ]]; then
    say "✗" "wire::${name} has no umbrella header"
    missing=$((missing + 1))
  fi
done
if [[ ${missing} -eq 0 ]]; then
  say "✓" "every wire protocol has an umbrella header"
else
  failures=$((failures + missing))
fi

# The size rule, from docs/STYLE.md: "Target 200 lines; treat 300 as a smell and 400 as a defect."
#
# A defect is a defect whoever wrote it, and five files had crossed the line — one of them written the same week
# the rule was quoted at somebody. Enforcing it here means the next one fails a build instead of accumulating.
oversize="$(find "${here}/include" "${here}/tests" "${here}/tools" "${here}/bench" "${here}/fuzz" \
  \( -name '*.hpp' -o -name '*.cpp' \) -exec wc -l {} + 2>/dev/null \
  | awk '$1 > 400 && $2 != "total" { print "      " $1 "  " $2 }' || true)"
if [[ -n "${oversize}" ]]; then
  say "✗" "files over 400 lines, which docs/STYLE.md calls a defect:"
  printf '%s\n' "${oversize}"
  failures=$((failures + 1))
else
  say "✓" "no file is over 400 lines"
fi

# The benchmark table is generated from the JSON it claims to come from, and two figures had drifted.
if python3 "${here}/scripts/sync-benchmark-table.py" --check >/dev/null 2>&1; then
  say "✓" "the benchmark table matches bench/results-*.json"
else
  say "✗" "docs/BENCHMARKS.md disagrees with bench/results-*.json — run scripts/sync-benchmark-table.py"
  failures=$((failures + 1))
fi

# Namespace count, which the README got wrong by two.
namespaces="$(find "${here}/include/dfr" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')"
words=(zero one two three four five six seven eight nine ten eleven twelve)
if grep -qiE "All (${words[*]// /|}) namespaces" "${here}/README.md" &&
   ! grep -qi "All ${words[${namespaces}]} namespaces" "${here}/README.md"; then
  say "✗" "the README miscounts the ${namespaces} namespaces"
  failures=$((failures + 1))
else
  say "✓" "the README counts ${namespaces} namespaces"
fi

# The viewer README has to mention what the page actually opens with. It once described a deleted heading.
for token in hero Findings "What broke"; do
  if ! grep -qiF "${token}" "${here}/viewer/README.md"; then
    say "✗" "viewer/README.md does not mention \"${token}\", which is on the page"
    failures=$((failures + 1))
  fi
done

if [[ ${failures} -ne 0 ]]; then
  echo "check-docs: ${failures} checks failed" >&2
  exit 1
fi
echo "check-docs: the documentation and the build agree"

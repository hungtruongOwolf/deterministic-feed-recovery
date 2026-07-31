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

if [[ ${failures} -ne 0 ]]; then
  echo "check-docs: ${failures} checks failed" >&2
  exit 1
fi
echo "check-docs: the documentation and the build agree"

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
# about *protocols*(the thing an external consumer includes by name, "give me SoupBinTCP") and `dfr::book` with
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

# The test badge is a number in a URL, which is the least likely thing anybody re-reads.
if grep -q "tests-${count}%20across" "${here}/README.md"; then
  say "✓" "the README badge says ${count} tests"
else
  say "✗" "the README test badge is stale; it should say ${count}"
  failures=$((failures + 1))
fi

# A portfolio repository with no licence is legally all rights reserved, which contradicts the reason it exists.
if [[ -f "${here}/LICENSE" ]]; then
  say "✓" "there is a licence file"
else
  say "✗" "there is no LICENSE file"
  failures=$((failures + 1))
fi

# The size rule, from docs/STYLE.md: "Target 200 lines; treat 300 as a smell and 400 as a defect."
#
# A defect is a defect whoever wrote it, and five files had crossed the line: one of them written the same week
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
  say "✗" "docs/BENCHMARKS.md disagrees with bench/results-*.json: run scripts/sync-benchmark-table.py"
  failures=$((failures + 1))
fi

# Namespace count, which the README got wrong by two.
namespaces="$(find "${here}/include/dfr" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')"
words=(zero one two three four five six seven eight nine ten eleven twelve)
# Joined with a real pipe. `${words[*]// /|}` looks like it builds an alternation and does not: bash applies the
# substitution to each element and then joins with a space, and since no element contains a space the result is
# "zero one two ...". Both counting guards below used it, so both grepped for a pattern that cannot match, took the
# else branch, and printed a tick while checking nothing. Found by doing what the last guard taught me to do:
# breaking the thing on purpose to watch the check fail, and watching it pass instead.
alternatives="$(IFS='|'; echo "${words[*]}")"
if grep -qiE "All (${alternatives}) namespaces" "${here}/README.md" &&
   ! grep -qi "All ${words[${namespaces}]} namespaces" "${here}/README.md"; then
  say "✗" "the README miscounts the ${namespaces} namespaces"
  failures=$((failures + 1))
else
  say "✓" "the README counts ${namespaces} namespaces"
fi

# The defect count. The README said "nine defects" the moment a tenth was added, and the defects are the strongest
# thing on the page: an undercount there is the one number a reader would have been given for free.
findings="$(grep -cE '^    kind: ' "${here}/viewer/src/model/findings.ts" | tr -d ' ')"
if grep -qE "\*\*[^*]+\*\*: (${alternatives}) defects" "${here}/README.md" &&
   ! grep -qE ": ${words[${findings}]} defects" "${here}/README.md"; then
  say "✗" "the README miscounts the ${findings} defects on the page"
  failures=$((failures + 1))
else
  say "✓" "the README counts ${findings} defects"
fi

# The viewer README has to mention what the page actually opens with. It once described a deleted heading.
for token in hero Findings "What broke"; do
  if ! grep -qiF "${token}" "${here}/viewer/README.md"; then
    say "✗" "viewer/README.md does not mention \"${token}\", which is on the page"
    failures=$((failures + 1))
  fi
done

# No em dashes, anywhere. A house rule rather than a matter of taste: the character was everywhere in this
# repository, in code comments, docs and on the page, and it reads as one writer's tic rather than as prose. A
# rule nobody can check is a preference, so it is checked. Colons, commas and full stops say the same things.
# Built at run time from its code point, and the escape written in three pieces, so this line does not itself
# contain the character it is banning. It also catches the JavaScript escape for it, which renders as one on the page.
em="$(printf '\xe2\x80\x94')"
dashes="$(grep -rIlE "${em}|.u20""14" "${here}" \
  --exclude-dir=build --exclude-dir=node_modules --exclude-dir=.git --exclude-dir=dist 2>/dev/null || true)"
if [[ -n "${dashes}" ]]; then
  say "✗" "em dashes are back in: $(echo "${dashes}" | tr '\n' ' ')"
  failures=$((failures + 1))
else
  say "✓" "no em dashes"
fi

if [[ ${failures} -ne 0 ]]; then
  echo "check-docs: ${failures} checks failed" >&2
  exit 1
fi
echo "check-docs: the documentation and the build agree"

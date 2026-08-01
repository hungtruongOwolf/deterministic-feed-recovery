#!/usr/bin/env bash
# Keeps the test count on the page equal to the test count in the build.
#
# A number typed into a component is a number that becomes a lie the next time a suite is added, and the viewer
# cannot run ctest to find out. So it is generated, and this script is what CI runs to check it did not drift,
# which makes the page's most useful sentence self-maintaining rather than a claim somebody has to remember.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-dev}"
model="${here}/viewer/src/model/findings.ts"

if [[ ! -d "${here}/build/${preset}" ]]; then
  echo "sync-test-count: build/${preset} is not configured" >&2
  exit 1
fi

count="$(ctest --test-dir "${here}/build/${preset}" -N | sed -n 's/^Total Tests: //p')"
if [[ -z "${count}" ]]; then
  echo "sync-test-count: could not read a test count from ctest" >&2
  exit 1
fi

current="$(sed -n 's/^export const TEST_COUNT = \([0-9]*\);$/\1/p' "${model}")"
if [[ "${current}" == "${count}" ]]; then
  echo "sync-test-count: ${count} tests, and the page agrees"
  exit 0
fi

if [[ "${1:-}" == "--check" || "${2:-}" == "--check" ]]; then
  echo "sync-test-count: the page says ${current} tests and the build has ${count}" >&2
  exit 1
fi

sed -i.bak "s/^export const TEST_COUNT = [0-9]*;$/export const TEST_COUNT = ${count};/" "${model}"
rm -f "${model}.bak"
echo "sync-test-count: updated the page from ${current} to ${count}"

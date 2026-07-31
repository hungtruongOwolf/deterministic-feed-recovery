#!/usr/bin/env bash
# Run the tests with real threads in them until they fail, or until enough runs agree.
#
# Why this exists
# ---------------
# The threaded book test aborted intermittently. I read "720 tests passed", shipped, and CI failed — then, looked
# for on purpose, it reproduced on the second run. So "the suite passed" had meant "the suite passed once", which
# for a test whose subject is an interleaving is close to no information at all.
#
# The abort was in the harness rather than the library: a Catch2 REQUIRE on the consumer thread races the main
# thread's result capture. That is exactly the class of defect a single run hides and a loop finds in seconds.
#
# How many runs, and why that number
# ----------------------------------
# Measured rather than picked. With the defect planted back, the test failed 4 times in 200 runs — a 2% rate, at
# 14 ms a run. So the first count I tried, 40, would have caught it 55% of the time: a guard that is a coin flip is
# worse than none, because a pass from it means nothing and gets believed anyway. I found that the honest way,
# by planting the bug and watching 40 runs report success.
#
# At 2%, 400 runs catch it with probability 99.97% and cost about six seconds. That is the trade this default makes.
# A rarer defect needs more, and the count is an argument here so raising it is a decision rather than a guess.
#
# Catch2 tags are not ctest labels in the vendored Catch.cmake — ADD_TAGS_AS_LABELS is silently ignored — so this
# filters by tag at the binary instead, which also works with no build system present.
set -euo pipefail

preset="${1:-dev}"
runs="${2:-400}"
tests="build/${preset}/tests"

[[ -d "$tests" ]] || { echo "hammer-concurrency: no build at $tests — configure and build that preset first" >&2; exit 1; }

# Every suite that starts a thread. Named rather than globbed: a new threaded suite should have to be added here
# deliberately, and a renamed one should fail loudly rather than quietly stop being hammered.
declare -a subjects=(
  "${tests}/dfr_concurrent_tests|"
  "${tests}/dfr_integration_tests|[concurrent]"
)

for subject in "${subjects[@]}"; do
  binary="${subject%%|*}"
  filter="${subject##*|}"
  [[ -x "$binary" ]] || { echo "hammer-concurrency: $binary is missing" >&2; exit 1; }
  name="$(basename "$binary")${filter:+ ${filter}}"
  printf 'hammer-concurrency: %s × %s\n' "$name" "$runs"
  for ((i = 1; i <= runs; ++i)); do
    if ! output="$("$binary" ${filter:+"$filter"} 2>&1)"; then
      printf '\n  ✗ %s failed on run %d of %d\n\n%s\n' "$name" "$i" "$runs" "$output" >&2
      exit 1
    fi
  done
  printf '  ✓ %s agreed %s times\n' "$name" "$runs"
done

echo "hammer-concurrency: the threaded tests hold under repetition"

#!/usr/bin/env bash
# What "730 tests pass" cannot say: which lines they never touch.
#
# A test count is a count, not a map. It says nothing about which branch of which function no test has ever
# reached, and the two real defects docs/COVERAGE.md records were both found by looking at the map rather than
# the count: chaos::moldudp64_target's four functions were reachable from a compile-time concept check and from
# nowhere else, and venue::order_session_state's two name_of() functions had no test at all, unlike every
# sibling enum-to-string function in this codebase.
#
# Source-based coverage, via the `coverage` CMake preset (-fprofile-instr-generate -fcoverage-mapping) rather
# than gcov: it reports per-region rather than per-line, which is the difference between "this branch of the
# ternary never ran" and "this line ran", and Xcode's toolchain ships llvm-cov even on a machine with no real
# GCC and no clang-tidy.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${here}/build/coverage"

# Resolved rather than assumed on PATH: this machine's clang-tidy investigation already found that neither
# Homebrew nor a bare `clang-tidy` exists here, and llvm-cov is the same story: it exists only inside the
# Xcode toolchain, under a name `xcrun` knows and a bare shell does not.
llvm_cov="$(xcrun --find llvm-cov 2>/dev/null || command -v llvm-cov || true)"
llvm_profdata="$(xcrun --find llvm-profdata 2>/dev/null || command -v llvm-profdata || true)"
if [[ -z "${llvm_cov}" || -z "${llvm_profdata}" ]]; then
  echo "coverage: llvm-cov/llvm-profdata not found (checked xcrun and PATH)" >&2
  exit 1
fi

echo "coverage: configuring and building"
cmake --preset coverage >/dev/null
cmake --build --preset coverage -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

rm -rf "${build}/profraw"
mkdir -p "${build}/profraw"

echo "coverage: running the suite instrumented"
# %p, not a fixed name: every test binary is a separate process, and without the pid each one would overwrite
# the last one's profile instead of adding to it.
LLVM_PROFILE_FILE="${build}/profraw/%p.profraw" ctest --test-dir "${build}" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

profraw_count="$(find "${build}/profraw" -name '*.profraw' | wc -l | tr -d ' ')"
if [[ "${profraw_count}" -eq 0 ]]; then
  echo "coverage: no .profraw files were written; is the coverage preset actually instrumented?" >&2
  exit 1
fi
echo "coverage: merging ${profraw_count} profiles"
"${llvm_profdata}" merge -sparse "${build}"/profraw/*.profraw -o "${build}/merged.profdata"

# Every test binary the build produced, not a hand-maintained list: a new test suite must be counted without
# anybody remembering to add it here.
#
# A while-read loop rather than `mapfile`: this machine's default /bin/bash is 3.2 (Apple ships the last
# GPLv2 release rather than a GPLv3 one), and `mapfile` is a bash-4 builtin that simply does not exist there.
bins=()
while IFS= read -r bin; do
  bins+=("${bin}")
done < <(find "${build}/tests" -maxdepth 1 -type f -perm -u+x | sort)
if [[ ${#bins[@]} -eq 0 ]]; then
  echo "coverage: no test binaries found under ${build}/tests" >&2
  exit 1
fi
cov_args=("${bins[0]}")
for bin in "${bins[@]:1}"; do
  cov_args+=(-object "${bin}")
done

# Restricted to the library's own headers: Catch2, the C++ standard library and the test files themselves are
# not what this measures, and reporting on them would bury the one thing worth reading in noise.
echo
"${llvm_cov}" report "${cov_args[@]}" -instr-profile="${build}/merged.profdata" \
  -ignore-filename-regex='(_deps|tests/)'

echo
echo "coverage: per-line detail for one file: llvm-cov show ${cov_args[0]} $(printf '%s ' "${cov_args[@]:1}")-instr-profile=${build}/merged.profdata <path-under-include/dfr>"
echo "coverage: see docs/COVERAGE.md for what was found the one time this was read closely, and for the"
echo "coverage:   known tooling limitation (a 'functions have mismatched data' warning that can put a"
echo "coverage:   function genuinely exercised by tests at 0% if a different translation unit only ever"
echo "coverage:   used it in a constant-evaluated context)."

# Coverage

"730 tests pass" is a count, and a count says nothing about which line, which branch, which function no test
has ever reached. The first time this project measured that directly, it found two real gaps: a fault-injection
target reachable only from a compile-time concept check, and an enum-to-string pair with no test at all while
every sibling of the same shape had one. Neither would have surfaced from reading the code, because in both
cases every individual line of code was correct: the gap was in what called it.

## Running it

```sh
./scripts/coverage.sh
```

Configures the `coverage` preset (`-fprofile-instr-generate -fcoverage-mapping` on Clang, `--coverage` on GCC),
builds, runs the whole suite with `LLVM_PROFILE_FILE` set so every test binary writes its own `.profraw`, merges
them with `llvm-profdata`, and prints an `llvm-cov report` restricted to `include/dfr/`. Source-based coverage
rather than gcov: it reports per-region, which is the difference between "this branch of the ternary never ran"
and "this line ran", and Xcode's toolchain ships `llvm-cov` on a machine that has neither Homebrew nor a real
GCC nor `clang-tidy`.

## What it found

**`chaos::moldudp64_target`'s four functions had never executed.** `target.hpp`'s own header comment states
"one injector drives both MoldUDP64 and IEX-TP" as the reason the whole policy-template design exists, and
`injector_test.cpp` has a `STATIC_REQUIRE(fault_target<moldudp64_target>)` proving the four functions exist with
the right signatures. Every actual fault-injection test in the suite constructs `chaos::injector<iextp_target>`;
nothing ever constructed `chaos::injector<moldudp64_target>` and ran a byte through it. The big-endian sequence
add, the session-bytes write, the saturating count and length adjustments: all untested arithmetic, reachable
only through a concept check that verifies signatures and nothing else.

Fixed in `tests/chaos/moldudp64_target_test.cpp`: the same four fault operations the IEX-TP tests exercise,
run against `chaos::injector<moldudp64_target>`, each verified by decoding the mutated packet afterward rather
than by counting changed bytes. Confirmed against a planted defect (`current - delta` instead of `current +
delta` in the sequence add): the test fails on the broken version and passes on the fix.

**`venue::order_session_state`'s two `name_of()` functions had no test.** Every other enum-to-string function in
this codebase (the arbiter's, the client's, the requester's, the snapshot planner's) has a direct unit test
asserting the exact string for every value. `name_of(session_phase)` and `name_of(session_ending)` did not; they
were called only from `tools/session.cpp`. That is also the function that puts these exact strings into the
trace JSON `tools/support/trace_writer.hpp` writes and the viewer reads, so a swapped case in either switch would
have shown up as a wrong word on the live page with nothing here to catch it first.

Fixed in `tests/venue/order_session_state_test.cpp`: one assertion per enum value, both enums. Confirmed against
a planted defect (swapped return string for `established`): fails on the broken version, passes on the fix.

## What was not chased, and why

The report shows plenty of files below 90% that this pass left alone, and the reason is the same in most cases:
**a `DFR_UNREACHABLE` branch for an enum's `count_` sentinel.** Every `name_of()` function in this codebase ends
with one, and testing it would mean calling the function with a value that is not a real enum member: the exact
class of "should never happen" case `DFR_UNREACHABLE` exists for, and the same reason death-tests exist
separately rather than being folded into ordinary unit tests. `recovery/client_state.hpp`'s two `name_of()`
functions are both directly tested (`tests/recovery/client_test.cpp`) and still show 59% region coverage for
exactly this reason; `order_session_state.hpp` now matches that pattern rather than exceeding it, which is the
right outcome, not a remaining gap.

Two lower-level numbers are worth naming rather than chasing for the same reason: `core/assert.hpp` at 45% and
`chaos/target.hpp`'s IEX-TP-side truncation checks are both dominated by paths that only execute when an
assertion or a bounds check actually fires, and this project's death-tests (`tests/support/death_test.hpp`) run
those paths in a forked child process that a signal terminates, which does not necessarily flush `.profraw`
data before the process ends. That is a property of the profiling runtime, not of the tests: the assertion
failure path is exercised, `ctest` reports it as passing, and the coverage instrumentation simply may not see
it. Chasing this number to 100% would mean either abandoning fork-based death-tests (losing the ability to
assert a specific crash *reason*, which is the entire point of the harness in `docs/DESIGN.md`) or building a
flush-before-abort mechanism whose only customer is the coverage report. Neither is worth it.

## A tooling limitation worth naming

`llvm-cov report` prints `warning: 495 functions have mismatched data` on this codebase, and it is not noise.
`capture/pcapng/constants.hpp`'s `pad4()` reported flat 0% coverage on the first run of this tool, and the grep
that should have settled the question the other way did: `tests/capture/pcapng_reader_test.cpp` calls
`minimal("x", true, /*tsresol=*/6)` and `/*tsresol=*/9`, both of which require padding a one-byte option and so
must call `pad4()` with a non-multiple-of-four length. The function is tested. The tool said otherwise.

The likely cause: `pad4()` is `constexpr` and is also used inside a `static_assert` elsewhere in the same header,
so different translation units hold different ideas of which region-mapping is canonical for the "same"
function, and `llvm-cov` picked one that was never instrumented at runtime. This is a known class of issue with
source-based coverage over header-only, `constexpr`-heavy C++ (not specific to this codebase) and the practical
consequence is: **a 0% or low number from this tool on a `constexpr` function is a prompt to grep for its
callers before believing it, not a verdict on its own.** Both real gaps this pass found and fixed were confirmed
by that grep before a line of test code was written; the one file that looked equally damning and was not is
recorded here so the next reading of this report does not repeat the investigation from zero.

## The numbers, as of this reading

| | region | function | line | branch |
|---|---|---|---|---|
| before this pass | 78.55% | 93.66% | 82.88% | 79.24% |
| after | 78.43% | 94.47% | 83.73% | 79.13% |

Region and branch coverage barely moved, expected, since the fixes closed two specific, narrow gaps rather
than chasing every low number, and the file recording the largest untouched gaps (`DFR_UNREACHABLE` branches)
is exactly the file that should not move. Function coverage is the number that mattered here: it went from
"four real functions and two real functions have never run" to zero such functions found, which is the actual
claim this pass set out to check.

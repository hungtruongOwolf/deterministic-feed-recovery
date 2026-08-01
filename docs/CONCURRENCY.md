# Concurrency

Where the threads are, why they are not anywhere else, and the two things measuring them changed my mind
about.

## The core is single-threaded, and that is load-bearing

`dfr::recovery` and `dfr::venue` are single-threaded and will stay that way. This is not caution. Determinism
is the property the whole project rests on(a failing run has to replay from a seed) and a multi-threaded
core would make the thread interleaving part of the input. There would be nothing left to reproduce.

But a feed handler that never leaves its own thread is not a feed handler. Somebody downstream consumes what
recovery delivers, and in every real system that somebody is on another core. So the concurrency sits exactly
where production systems put it: **one seam**, between the thread that owns the protocol state and the thread
that owns the strategy. `dfr::concurrent::spsc_ring` is that seam. The core never learns a second thread
exists.

## Full means refused, never overwritten

When the consumer falls behind, `push` fails and the ring counts it. It does not overwrite the oldest record.
That is the same decision `recovery::replay_buffer` and `trace::recorder` make, for the same reason applied to
a third place: overwriting turns a *known* backlog into a silent hole, and the oldest record is the one whose
loss is hardest to notice.

A dropping ring is defensible for some feeds(a stale quote is worthless) but it has to be the caller's
choice, made where the caller can account for it. `refused()` is how this one accounts.

## What it costs

Apple M-series, two cores of one machine, `-O3 -flto`, 6–8 million messages per run, three runs.

| | ns per message | messages/s |
|---|---|---|
| 272-byte records, one `pop` each | ~26 | ~38 M |
| 272-byte records, `pop_batch` of 64 | **~13** | **~76 M** |
| 8-byte records, padded indices | ~39 | ~26 M |
| 8-byte records, indices sharing a line | ~45 | ~22 M |
| consumer deliberately too slow | ~350 | ~2.8 M, **10.3 M refused** |

The last row is the design decision working: the producer was refused ten million times and the ring can say
so. A dropping ring would have reported the same throughput and lost the same data silently.

### Two things the measurements changed

**Padding is worth 10–25%, not an order of magnitude.** I wrote "the single most expensive mistake available
here, costs an order of magnitude" in the header before measuring it, because that is what the received wisdom
says. Measured, an unpadded ring is consistently slower(in all three runs, on both record sizes) by 10% to
25%. Real, worth having, and not the number I would have quoted from memory.

**And the first attempt at measuring it found the unpadded ring *faster*,** which was not a result about
padding at all. With 272-byte records, copying the record costs more than the cache line the two indices fight
over, so the fight is invisible underneath it. Isolating the effect needs an 8-byte record. That is also the
honest statement of when the padding earns its keep: **when the records are small and the ring is hot.** A
design note that says "pad the indices" without saying when is a cargo cult.

**Batching is the bigger win.** `pop_batch(64)` is about twice as fast per message as `pop` in a loop, because
one core-to-core cache-line transfer serves the whole batch instead of one per record. The floor underneath
everything here is that transfer latency(tens of nanoseconds on this machine) and no arrangement of the
producer's code gives it back. Consuming one at a time pays for a round trip per message.

## ThreadSanitizer passes a broken version of this ring

This is the finding worth reading twice, and it is why `docs/BENCHMARKS.md`'s ledger has a
`not-measurable` row about memory ordering.

The experiment: replace `memory_order_release` and `memory_order_acquire` on the two indices with
`memory_order_relaxed`, which makes the ring **wrong**: the index can become visible before the slot's bytes,
so the consumer can read a half-written record.

| check | result on the broken ring |
|---|---|
| the `tsan` preset, 200,000 messages | **passes**: no warning at all |
| the property test, `-O3`, arm64, 12 runs | **fails 12 / 12** |

ThreadSanitizer does not model relaxed atomics precisely: it establishes happens-before on atomic operations
regardless of the order declared, so it cannot tell a sufficient ordering from an insufficient one. It is
excellent at finding *unsynchronised plain memory*, which is a different bug.

What caught it is the property test: every record carries the sequence the producer put in it, they arrive in
order, none missing or repeated: running on **weakly-ordered hardware**. This machine is arm64, which
reorders; on x86-64 the same broken code would very likely pass, because x86's memory model happens to provide
the ordering the code failed to ask for.

That inverts the usual arrangement in this project. Normally the local machine is the narrow one and CI's
second compiler catches what it cannot (see `docs/DESIGN.md`). Here **the local arm64 machine is the only
place this bug is visible**, and a CI running on x86 Linux would report a clean build on broken code. So:

- the ordering argument is written out in `spsc_ring.hpp` rather than assumed;
- the property test is the guard, not the sanitiser;
- and the claim "the memory ordering is sufficient" is marked **not measurable** in the ledger, because a
  passing test on one machine is not a proof and saying otherwise would be the kind of claim this project
  exists to avoid making.

A real proof needs a model checker over the memory model: CDSChecker, GenMC, or a herd7 litmus test. That is
not built here, and not pretending to be is the point.

## A concurrency test that has passed once has told you almost nothing

The threaded book test(*the book built across a thread boundary is the same book*) aborted intermittently in
CI with a `SIGABRT`, no failing expression, and a Catch2 internal assertion:

```
Assertion `!m_redirectActive && "redirect is already active"' failed.
```

I had read "723 tests passed" locally and pushed. Looked for on purpose afterwards, it reproduced on the **second
run** on the same machine. So it had never been platform-specific; "the suite passed" had meant "the suite passed
once", which for a test whose subject is an interleaving is close to no information at all.

**The defect was in the measuring apparatus.** `detail::apply` in the test support used `REQUIRE`, and the threaded
test calls it from the consumer thread. Catch2's result capture and output redirect are single-threaded, so two
threads inside them race and abort. Nothing in `dfr::concurrent` was wrong, which is the worst shape a flake can
have, because it points at the library and wastes the reading on the wrong file.

The fix is that the shared apply function counts decode failures into `replay_result::malformed` and the main thread
asserts it is zero after the join. Sharing one apply function between the two replays is the point of that seam and
it survives; what it may not do is touch Catch2.

### Choosing the repetition count by measuring, not by feel

`scripts/hammer-concurrency.sh` runs the threaded suites until they fail or until enough runs agree. With the defect
planted back:

| runs | probability of catching a 2% flake | cost |
|---|---|---|
| 40 | 55% | ~1 s |
| 120 | 91% | ~2 s |
| **400** | **99.97%** | **~6 s** |

The rate is measured: 4 failures in 200 runs, at 14 ms a run. My first count was 40, and it **reported success on
the planted bug**: a guard that is a coin flip is worse than none, because its pass gets believed anyway. At 400
it caught the planted bug on run 167, and the fixed version agreed 400 times.

CI runs it on `dev` and `release`, and not under the sanitisers, where a single run already inspects far more than
repetition would and 400 would take longer than the rest of the workflow put together.

One incidental finding: Catch2 3.7.1's `catch_discover_tests` **silently ignores** `ADD_TAGS_AS_LABELS`. The first
version of this guard used `ctest -L concurrent`, which selected nothing and reported `Total Tests: 0` rather than an
error: a guard that runs zero tests and exits zero. It filters by tag at the binary instead.

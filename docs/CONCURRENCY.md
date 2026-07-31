# Concurrency

Where the threads are, why they are not anywhere else, and the two things measuring them changed my mind
about.

## The core is single-threaded, and that is load-bearing

`dfr::recovery` and `dfr::venue` are single-threaded and will stay that way. This is not caution. Determinism
is the property the whole project rests on — a failing run has to replay from a seed — and a multi-threaded
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

A dropping ring is defensible for some feeds — a stale quote is worthless — but it has to be the caller's
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
says. Measured, an unpadded ring is consistently slower — in all three runs, on both record sizes — by 10% to
25%. Real, worth having, and not the number I would have quoted from memory.

**And the first attempt at measuring it found the unpadded ring *faster*,** which was not a result about
padding at all. With 272-byte records, copying the record costs more than the cache line the two indices fight
over, so the fight is invisible underneath it. Isolating the effect needs an 8-byte record. That is also the
honest statement of when the padding earns its keep: **when the records are small and the ring is hot.** A
design note that says "pad the indices" without saying when is a cargo cult.

**Batching is the bigger win.** `pop_batch(64)` is about twice as fast per message as `pop` in a loop, because
one core-to-core cache-line transfer serves the whole batch instead of one per record. The floor underneath
everything here is that transfer latency — tens of nanoseconds on this machine — and no arrangement of the
producer's code gives it back. Consuming one at a time pays for a round trip per message.

## ThreadSanitizer passes a broken version of this ring

This is the finding worth reading twice, and it is why `docs/BENCHMARKS.md`'s ledger has a
`not-measurable` row about memory ordering.

The experiment: replace `memory_order_release` and `memory_order_acquire` on the two indices with
`memory_order_relaxed`, which makes the ring **wrong** — the index can become visible before the slot's bytes,
so the consumer can read a half-written record.

| check | result on the broken ring |
|---|---|
| the `tsan` preset, 200,000 messages | **passes** — no warning at all |
| the property test, `-O3`, arm64, 12 runs | **fails 12 / 12** |

ThreadSanitizer does not model relaxed atomics precisely: it establishes happens-before on atomic operations
regardless of the order declared, so it cannot tell a sufficient ordering from an insufficient one. It is
excellent at finding *unsynchronised plain memory*, which is a different bug.

What caught it is the property test — every record carries the sequence the producer put in it, they arrive in
order, none missing or repeated — running on **weakly-ordered hardware**. This machine is arm64, which
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

A real proof needs a model checker over the memory model — CDSChecker, GenMC, or a herd7 litmus test. That is
not built here, and not pretending to be is the point.

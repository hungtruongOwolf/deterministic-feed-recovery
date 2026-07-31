# Benchmarks

What the recovery path costs, and — more carefully — what these numbers do not say.

This document exists because the project had a build preset called `bench` for months that turned assertions
off and **measured nothing**. For a library whose reason to exist is a hot path that runs when something has
already gone wrong, that was the largest hole in it: "no allocation after init" and "poll-driven" were design
claims with no number attached to either.

## Running them

```sh
cmake -S . -B build/o3-off -DCMAKE_BUILD_TYPE=Release -DDFR_ASSERTIONS=off
cmake --build build/o3-off --target dfr_recovery_bench -j8
./build/o3-off/bench/recovery_bench --samples 400 --json bench/results.json
```

`scripts/run-benchmarks.sh` builds all three assertion levels at a fixed optimisation level and writes the
JSON the viewer draws.

## The numbers

Apple M-series laptop, single core, `-O3 -flto`, 400 samples per measurement. **Every figure moves on
different hardware**; the ratios between them move much less.

| operation | assertions off | fast | paranoid |
|---|---|---|---|
| decode an IEX-TP header | **1.9 ns** | 7.9 | 8.7 |
| frame a MoldUDP64 packet and walk every message | **4.1 ns** | 8.1 | 7.4 |
| gap-set arithmetic, per open or fill over 8 holes | **32.6 ns** | 35.8 | 41.0 |
| ingest a packet end to end, clean feed | **51.3 ns** | 39.1 | 64.9 |
| ingest with loss, and poll | **84.3 ns** | — | 112.3 |
| allocations after initialisation | **0** | 0 | 0 |

At 8 messages per packet, the clean-feed figure is roughly **19.5 million packets or 156 million messages a
second on one core**, with no I/O in the loop.

### What the assertion columns are for

The comparison is the interesting part, and getting it honest took a correction. `dev` is `Debug` with
paranoid assertions and `bench` is `Release` with them off, so comparing those two prices **two variables at
once** — optimisation level and assertion level — and reports the total as though it were the cost of the
assertions. It gave a 55× ratio on the header decode, which is a real number about nothing anybody would
ship.

The table above holds `-O3 -flto` fixed and moves only `DFR_ASSERTIONS`. Paranoid assertions cost about
**4.6× on the tightest operation** and about **30% on the realistic hot path** — which is what you would
expect, because the tight operation is almost entirely bounds checks and the hot path is mostly work the
assertions do not touch. That is a number worth knowing before choosing what to ship.

## What these numbers are

Nanoseconds per operation, derived from **batch means**: each sample times a batch of operations and divides.
`steady_clock` resolves to tens of nanoseconds on this machine and several of these operations cost less than
that, so timing one individually would measure the clock.

The consequence is worth stating plainly: **the percentiles are over batch means, not over individual
operations.** A p99 over batch means cannot show a single ten-microsecond stall inside a batch of a thousand.
It can show that some batches ran consistently slower than others, which is what scheduler noise and cache
state look like. If you need a true per-operation tail you need hardware timestamps, and see below.

## What these numbers are not

**Not tick-to-trade. Not NIC-to-NIC. Not any wire latency at all.** Every figure here is CPU time inside one
process with no network in the loop. Measuring wire latency needs NIC hardware timestamping and PMU counters,
and the machines this project runs on — a laptop and cloud VMs — have neither. So no figure is given rather
than a figure with nothing behind it. This is the same rule the run traces follow, and it is in the JSON
output so a page drawing these cannot present one as the other.

**Not a comparison with a production feed handler.** There are no published figures to compare against, and a
different machine would move every number in the table.

**Not a claim about a whole system.** There is no I/O, no kernel bypass, no thread hand-off in the figures
above. The SPSC hand-off is measured separately, in `docs/CONCURRENCY.md`.

## Two measurement bugs worth recording

Both were in the flattering direction and both were found by reading the numbers rather than by any test.

**A poll that measured the compiler.** Polling a synchronised client reported `0.0 ns` and ninety-seven
billion polls a second. The client's state never changed, so the loop was invariant, and `-O3` computed one
answer and kept it. A `keep()` barrier on the loop's *result* does not help: the fix is a real data
dependency, so each iteration's input comes from the last iteration's output.

**A poll that measured a constructor.** The next version built a fresh client each iteration and reported
~594 ns for "one poll" — mostly the cost of zero-initialising a 53 KB object. The lesson is the unit: what is
timed has to be what the label says, and "one poll" and "construct a client and then poll" differ by an order
of magnitude.

**And one before those:** the gap-set benchmark built an eight-hole set inside the timed region and divided by
one, reporting ~950 ns for a single `open`. The unit is now a hole's whole life — eight opened then eight
filled, divided by sixteen — which is also what happens on a real feed.

A benchmark that can be constant-folded, or whose unit is wrong, produces a number with a decimal point and
no meaning. The three above are why every measurement here names its unit in the output.

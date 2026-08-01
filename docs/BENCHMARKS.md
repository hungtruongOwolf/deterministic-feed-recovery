# Benchmarks

What the recovery path costs, and(more carefully) what these numbers do not say.

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

Apple M-series laptop, single core, `-O3 -flto`, 400 samples per measurement, four rounds with the three
builds run in rotation, minimum per figure. Read from the committed `bench/results-*.json`.

**Why the minimum, and why rotation.** Noise on a benchmark is one-sided: a scheduler preemption, a thermal
ramp or a neighbouring process can only make an operation appear slower. Running one configuration to
completion before the next lets a thermal ramp land entirely on one of them, which is exactly how an earlier
table came out with `fast` assertions slower than `paranoid` and `paranoid` faster than `off`. That was not a
result, it was the afternoon. Rotating spreads the machine's mood across all three and the minimum discards
what is left.

**Every figure moves on different hardware.** The ratios move far less, which is why the last column is the
part worth quoting and the absolute nanoseconds are not.

| operation | off | fast | paranoid | paranoid cost |
|---|---|---|---|---|
| decode an IEX-TP header | **1.02 ns** | 3.01 | 2.97 | **2.9×** |
| frame and walk a MoldUDP64 packet | **2.20 ns** | 3.01 | 2.85 | **1.3×** |
| gap-set arithmetic, a hole's whole life | **14.97 ns** | 14.32 | 14.98 | below the noise floor |
| ingest with loss, and poll | **58.02 ns** | 59.33 | 59.08 | below the noise floor |
| ingest a packet end to end | **41.50 ns** | 41.83 | 42.32 | below the noise floor |
| allocations after initialisation | **0** | 0 | 0 | none |

At 8 messages per packet, the clean-feed figure is roughly **24 million packets or 193 million messages a
second on one core**, with no I/O in the loop.

### What the assertion columns actually showed

The obvious comparison is the `dev` and `bench` presets, and it is wrong: `dev` is `Debug` and `bench` is
`Release`, so it prices **two variables at once** and reports the sum as the cost of one. It gave a 55× ratio
on the header decode: a real number about a configuration nobody would ship.

Holding `-O3 -flto` fixed and moving only `DFR_ASSERTIONS` gives a more interesting and much less flattering
answer than the one I expected:

- **On the tightest operation, paranoid assertions cost 3×.** Decoding a 40-byte header is almost entirely
  bounds checks once the checks are on, so this is the shape to expect and the number is real.
- **On the realistic hot path, they cost nothing measurable.** Ingesting a packet end to end, and ingesting
  with loss and polling, differ by about 2% between assertions-off and assertions-paranoid, and the ordering
  *flips* between rounds, which is the signature of a difference smaller than the noise. Claiming a 2%
  improvement here would be claiming the weather.

The engineering conclusion is the useful part, and it is not the one the 55× number pointed at: **the paranoid
assertions can stay on in production on this path.** The work that dominates an ingest: arbitration, gap
arithmetic, watermark bookkeeping: is work the assertions do not touch, so the paranoia is free where it
matters and expensive only where the operation is nothing but checks. That is worth knowing, and no design
document could have told me.

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
and the machines this project runs on(a laptop and cloud VMs) have neither. So no figure is given rather
than a figure with nothing behind it. This is the same rule the run traces follow, and it is in the JSON
output so a page drawing these cannot present one as the other.

**Not a comparison with a production feed handler.** There are no published figures to compare against, and a
different machine would move every number in the table.

**Not a claim about a whole system.** There is no I/O, no kernel bypass, no thread hand-off in the figures
above. The SPSC hand-off is measured separately, in `docs/CONCURRENCY.md`.

## The one figure measured in the reader's browser

Everything above is measured natively and committed to `bench/results-*.json`, which makes all of it honest and none
of it something a visitor caused. A benchmark you cannot reproduce is a claim, so the viewer's performance section
opens with a figure the reader produces by pressing something: the page already recompiles the three acts into
WebAssembly and runs them on every settings change, and timing that call costs one `performance.now()` on each side.

It reports **minima, per message**, for the same reason the tables above do, and the reason was measured rather than
assumed. Back-to-back runs of the same three acts came out at 242,778 and then 570,982 messages a second, a 2.4×
spread with the *first* run the slowest: a cold WebAssembly instance and an unwarmed JIT are both being paid for.
Reporting the latest run would make the opening figure the worst one and lurch on every keystroke. Per message rather
than per run, because the length control changes how much work a run is.

The figure lands roughly **100× below** the native ingest number, and the page states that factor itself rather than
leaving a reader to compute it and mistrust both. The gap is not an embarrassment; it is the reason these tables
exist:

- WebAssembly, single-threaded, in a browser sandbox;
- every event serialised to JSONL as it runs, which is most of what is being timed: the trace format is the
  architecture, not overhead to be excused;
- and a browser cannot resolve a two-nanosecond operation at all.

So the honest claim is narrow, and it is the only one made: this many messages went through the real state machine,
on your machine, just now.

## What GCC said, by accident

A step that built C++ inside the viewer's CI job picked up the runner's default **GCC** rather than the pinned
Clang, and failed with warnings Clang does not have:

```
error: use of 'std::hardware_destructive_interference_size'   [-Winterference-size]
error: useless cast to type 'bool'                            [-Wuseless-cast]
error: useless cast to type 'std::size_t'                     [-Wuseless-cast]
```

The step was in the wrong job and has been moved. But the finding is real and is recorded rather than dropped:
GCC warns on `hardware_destructive_interference_size` because its value is part of the ABI and differs between
GCC versions, which is precisely the sort of thing a cache-line constant in a lock-free structure should not be
silently inheriting. `spsc_ring.hpp` already falls back to a literal 64 when the standard constant is absent;
what it does *not* yet do is prefer the literal even when the constant exists.

**Fixed, and the fix removed a dependency rather than silencing a warning.** GCC is right about
`hardware_destructive_interference_size`: its value is part of the GCC ABI, so two translation units built with
different GCC versions can disagree about how wide a padded member is: a layout mismatch in a lock-free
structure, not a performance question. A constant whose value depends on which compiler saw the header is not a
property of the machine, so `core/attributes.hpp` no longer uses it and states a literal per architecture: 128 on
arm64, 64 elsewhere. That is what the comment beside it had always argued for and what rigtorp's MPMCQueue does.

The two `-Wuseless-cast` findings were also real. `DFR_MAYBE` cast a bool to bool; it now uses a conditional,
which converts and casts nothing. `rng::index` casts `uint64_t` to `size_t`, which is a no-op on LP64 and
necessary on a 32-bit target: GCC is right on the first and wrong on the second, so a `narrowed<To>` helper makes
the choice at compile time instead of leaving it to be argued about.

**And there is now a GCC job**, because the local machine has no real GCC (Apple Clang answers to `g++`) and a
warning nobody sees is a warning that comes back.

## Two measurement bugs worth recording

Both were in the flattering direction and both were found by reading the numbers rather than by any test.

**A poll that measured the compiler.** Polling a synchronised client reported `0.0 ns` and ninety-seven
billion polls a second. The client's state never changed, so the loop was invariant, and `-O3` computed one
answer and kept it. A `keep()` barrier on the loop's *result* does not help: the fix is a real data
dependency, so each iteration's input comes from the last iteration's output.

**A poll that measured a constructor.** The next version built a fresh client each iteration and reported
~594 ns for "one poll": mostly the cost of zero-initialising a 53 KB object. The lesson is the unit: what is
timed has to be what the label says, and "one poll" and "construct a client and then poll" differ by an order
of magnitude.

**And one before those:** the gap-set benchmark built an eight-hole set inside the timed region and divided by
one, reporting ~950 ns for a single `open`. The unit is now a hole's whole life: eight opened then eight
filled, divided by sixteen, which is also what happens on a real feed.

A benchmark that can be constant-folded, or whose unit is wrong, produces a number with a decimal point and
no meaning. The three above are why every measurement here names its unit in the output.

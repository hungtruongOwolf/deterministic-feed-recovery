# Deterministic Feed Recovery

**DFR** — a market-data feed broken on purpose, and the C++20 client that puts it back together.

A seeded fault injector and a recovery library for exchange market-data feeds. Every run is a deterministic
function of its seed, so a failure is a number somebody else can type in and see for themselves.

**[Watch a run →](https://hungtruongowolf.github.io/deterministic-feed-recovery/)**

## Status

All seven namespaces are implemented and tested.

| | what it is | state |
|---|---|---|
| `dfr::core` | `result<T>`, errors, views, injected clocks | done |
| `dfr::wire` | MoldUDP64, IEX-TP, SoupBinTCP 3.00, OUCH 4.2 | done |
| `dfr::capture` | pcap reading, replayed against real IEX HIST files | done |
| `dfr::chaos` | seeded, protocol-aware fault injection | done |
| `dfr::recovery` | arbitration, gap tracking, retransmission, snapshot recovery, one poll-driven client | done |
| `dfr::venue` | market-data publisher, retransmit and snapshot facilities, OUCH order entry over a SoupBinTCP session | done |
| `dfr::trace` | recording a run as JSONL, and the viewer that reads it | done |

**676 tests pass under four configurations** — assertions at paranoid, fast and off, and
AddressSanitizer + UndefinedBehaviorSanitizer — all with warnings as errors, on Apple Clang locally and
Linux Clang in CI. There is an end-to-end oracle over both synthetic streams and real captures.

Not built, on purpose: a matching engine. Matching is the part 1,071 other C++ repositories already
implement; what is missing from the open-source world is the protocol behaviour *around* it, so executions
are driven by the caller and the host's job is to keep the accounting straight and emit the right messages.
See `include/dfr/venue/order_entry.hpp` for the argument.

## Try it

```sh
cmake -S . -B build/dev && cmake --build build/dev -j8

# One order-entry session, both directions of the wire.
# The last three lines are the point: the client counted the sequence itself.
./build/dev/tools/session

# All 676 tests, assertions at paranoid.
ctest --test-dir build/dev

# A run recorded as JSONL. The same seed gives byte-identical output; a different seed does not.
./build/dev/tools/trace --seed 4711 --messages 300 --faults 6 --out /tmp/a.jsonl
./build/dev/tools/trace --seed 4711 --messages 300 --faults 6 --out /tmp/b.jsonl && diff /tmp/a.jsonl /tmp/b.jsonl

# Against a real capture, if you have an IEX HIST pcap:
./build/dev/tools/inspect  <capture.pcap>
./build/dev/tools/verify   <capture.pcap> --seed 4711 --faults 40
```

Or [run it in the browser](https://hungtruongowolf.github.io/deterministic-feed-recovery/) — the page
compiles this library to WebAssembly, so the seed you type is a run that happens. Not a replay of a
recording.

```sh
# The browser build, and the check that matters: does it produce the same bytes as the terminal?
./scripts/build-wasm.sh
./scripts/check-wasm.sh dev
```

Two compilers, two targets, one seed. Six shapes of run — one line, two lines, the glimpse race, two session
scripts — diffed byte for byte. If they ever differ, something in the library depends on its platform and
"deterministic" was a word rather than a property, so CI fails on it.

## What this is meant to be

Three components under the namespace `dfr`, built in this order:

1. **`dfr::chaos`** — a seeded, protocol-aware fault injector for MoldUDP64 / IEX-TP multicast
   streams. Burst loss, reordering, duplication, A/B line divergence, sequence resets,
   snapshot/incremental races. A deterministic function of `(seed, packet_index)`, so any
   failure replays exactly.
2. **`dfr::recovery`** — a client library that survives all of the above: gap detection,
   retransmission requests, snapshot-based book reconstruction, A/B arbitration, NAK
   suppression.
3. **`dfr::venue`** — a mock exchange that speaks the real wire protocols, so `dfr::recovery`
   can be tested against something that behaves like an exchange rather than a stub. Market data
   out over IEX-TP, retransmission and snapshots that can refuse, and OUCH 4.2 order entry in.

"Deterministic" here is a constraint on the implementation, not a claim about its quality: no
wall clock, no unseeded randomness, no pointer-derived ordering, and a single-threaded core, so
that a failing run is reproducible from a seed plus a build fingerprint.

## Why this problem

Searched GitHub on 2026-07-29:

| Query | Repos |
|---|---|
| `"order book" language:C++ created:>2026-01-01` | 1,071 |
| …of those, with ≥5 stars | 7 |
| `"gap fill" multicast market data` | **0** |
| `glimpse soupbintcp` | **0** |
| `feed arbitration multicast market data` | 1 (0 stars) |

Feed *decoders* are saturated. The recovery path — the code that runs only when something has
already gone wrong — has no open-source implementation, and no tool exists to test one.

Supporting evidence that this is where the bugs are: Yuan et al., OSDI'14 found that 92% of
catastrophic system failures came from incorrect handling of errors that were explicitly
signalled in software, and that 58% could have been caught by simple testing of the
error-handling code.

## What a 400-seed sweep actually showed

The page reports two properties, and it reports them because they are what survived measurement rather than
what sounded good. Two earlier claims were tried and both were false:

| claim | held |
|---|---|
| nothing is ever delivered twice | **400/400** |
| nothing is lost until the last defence answers too late | **400/400** |
| two lines mean fewer messages needing a round trip | 399/400 |
| each act is forced one layer deeper than the last | fails often |

The two failures are the interesting part.

**"Each act reaches deeper" is false** at any seed where the second act's faults happen to close from a
retransmit the first act also needed — at seed 7 with six faults, the second act never asks for anything.

**"Two lines mean fewer round trips" is false in two different ways.** At seed 114 the second line fills the
*middle* of a hole, splitting one 27-message gap into two of 9 and 15: *more* requests, *fewer* messages.
And at seed 186 two lines need more messages back than one, because the injector damages each line
separately — redundancy is not a strict subset of the single-line failure, it is a different one.

So the run summary gained `retransmit_messages` alongside `retransmit_requests`, because the two answer
different questions, and the page states the third row as a tendency rather than a law. That distinction is
the whole point of the honesty ledger applied to a claim I wanted to make.

## Verified against real captures

The IEX-TP field offsets were transcribed from a specification whose live URL now
serves a "this document has moved" stub, so they had a single source. They have
been checked against real IEX HIST data with `tools/inspect`:

| Capture | Format | Frames | Messages | VLAN | Chain breaks |
|---|---|---|---|---|---|
| `20170826` DEEP, whole file | classic pcap | 20,145 | 48,635 | 1013 | **0** |
| `20191224` DEEP, first 12 MB of the gzip | pcapng | 348,103 | 380,611 | none | **0** |
| `20170923` DEEP, whole file | pcapng | 27,827 | 60,647 | none | **0** |
| `20180929` DEEP, whole file | pcapng | 23,258 | 59,239 | none | **0** |
| `20190907` DEEP, whole file | pcapng | 21,047 | 60,043 | none | **0** |
| `20241001` DEEP, whole file | classic pcap | 20,198 | 59,367 | none | **0** |

Zero chain breaks across 460,578 real packets means all three of IEX-TP's
redundant chains held on every one: sequence numbers chained, stream offsets
chained, and the block framing accounted for exactly the declared payload length.
A wrong offset for Stream Offset or Payload Length would have broken on the
second packet.

The last four rows are a re-verification, run after `chain_checker` was changed —
because a decoder that has been "verified once" and then edited is a decoder that
has not been verified.

```
$ curl -s 'https://iextrading.com/api/1.0/hist?date=20170826' | python3 -m json.tool
$ curl -L -o deep.pcap.gz '<the DEEP link>' && gunzip deep.pcap
$ inspect deep.pcap
```

## The end-to-end oracle

`tools/verify` injects a seeded fault schedule into a real capture, runs the damaged
stream through `dfr::recovery`, and plays retransmit server from the undamaged
original. It checks two properties and exits non-zero if either fails:

- **detection** — the messages the client reports missing are exactly the ones that
  never reached it, no more and no fewer;
- **repair** — with a retransmit server, nothing is missing at the end and every
  message was delivered exactly once.

```
$ verify deep_20170826.pcap --seed 4711
  IEX-TP packets usable    20145
  messages delivered       48635
  retransmits served       11
  reported missing         0
  actually never arrived   0
  delivered twice          0
  detection exact          yes
  every message once       yes
  accounting balances      yes
  fully repaired           yes
```

Both properties hold across **50 runs** — five captures spanning 2017 to 2024, both
container formats, ten seeds each. The message count matches `inspect`'s independent
count for the same file, which is a second opinion on the accounting.

The same oracle runs in CI over synthetic packets
(`tests/integration/recovery_oracle_test.cpp`), where it is fast, self-contained, and
fails on the commit that broke something. The synthetic stream carries heartbeats
because a stream without them let a real defect through: a heartbeat advances the
tracker's expectation without advancing the arbiter's watermark, and a retransmit
filling the resulting hole was counted twice. Only the real-data run found it.

Three things the exercise turned up:

- **The VLAN tag is not consistent across the corpus.** The 2017-08-26 file carries
  VLAN 1013; every other file sampled carries no tag. A reader tested against only
  one would break on the other and report the failure as "this file contains no IP
  traffic".
- **There is no format switch to find.** The first guess was a date boundary, since
  trading days from 2017-07-03 are pcapng while the 2017-08-26 Saturday file is
  classic pcap. Sampling further killed the theory outright: `20241001` is classic
  pcap too, seven years later, with a snaplen of 262,144 rather than 65,535. The
  format simply varies file to file, so a tool cannot pick a reader by date or by
  era — it has to try one and fall back, which is what `inspect` does.
- **The multicast group, port and session id all vary too** (233.215.21.4:10378 in
  2017, 233.215.21.242:32001 in 2024). Anything hard-coded from one capture is a
  parser that works on exactly one file.

## Recorded runs

`tools/trace` records a whole run — venue publishing, faults injected, the client's every
decision — as one JSON object per line. A trace is a deterministic function of the seed, so
it is committed next to the code rather than regenerated: `traces/` holds two, and
`diff`ing a fresh run against them is a behavioural regression test a human can read.

```
$ trace --seed 4711 --messages 300 --out run.jsonl
$ trace --glimpse --out glimpse.jsonl      # loses the Glimpse race on purpose
```

Every event carries the *resulting* client state and headline numbers. That redundancy is
deliberate: a viewer must be able to draw any moment by reading one line, because a viewer
that reconstructed state from the event sequence would be a second implementation of the
state machine — and when the two disagreed, the picture would be wrong with nothing to say
so.

The header also carries a generated `limits` array: which claims this run measured and which
cannot be measured on the hardware available. It is in the data rather than in prose so it
cannot drift from what the run actually did.

## Viewer

`viewer/` is a static page that reads a trace and draws it: a time scrubber over the packet axis,
the client's state as a band across the run, the Glimpse race drawn on a sequence axis, per-line
health for a redundant pair, and the honesty ledger.

**Live:** <https://hungtruongowolf.github.io/deterministic-feed-recovery/>

```
cd viewer && npm install && npm run dev
```

It contains **no domain logic**. Every number drawn is a field the trace already carries; nothing
is recomputed. A viewer that reconstructed state from the event sequence would be a second
implementation of the state machine in another language, and when the two disagreed the picture
would be wrong with nothing to say so — the exact failure this library exists to prevent,
reintroduced in the tool built to display it.

`scripts/regenerate-traces.sh` refreshes the committed fixtures; `git diff traces/` afterwards is a
behavioural regression report.

## Test data

Real wire-format captures, free and without registration:

- **IEX HIST** (`iextrading.com/api/1.0/hist`) — pcap with 802.1Q VLAN + IPv4 multicast +
  IEX-TP. Primary corpus.
- **`Open-Markets-Initiative/omi-data-pcaps`** — genuine NASDAQ MoldUDP64 multicast pcap.
- **B3 `MBO_EQT_Incremental_FeedA/FeedB`** — the only free A/B redundant capture pair found.

Note that NASDAQ's own free samples (`emi.nasdaq.com`) are BinaryFILE format: a 2-byte length
prefix plus raw ITCH, with the entire transport layer stripped. They carry no MoldUDP64 header,
no session ID and no packet sequence numbers, so they cannot be used to test gap handling
without synthesising the transport first — at which point the test exercises the synthesiser.

## Stated limits

- All development and measurement happens on cloud VMs. No PMU counters, no Intel PT, no NIC
  hardware timestamping, no reliable sub-microsecond clock. **Any timing number here is a
  software timestamp and should be read as such.** This project is about correctness and
  determinism, not about tick-to-trade latency.
- AWS does not support multicast on an ordinary VPC. Local testing is `veth` + network
  namespaces + `tc netem`, so IGMP snooping and querier behaviour — a common operational cause
  of a feed going silent — is reasoned about rather than reproduced.

## Building

```
cmake --preset dev       # paranoid assertions, no optimisation
cmake --build --preset dev
ctest --preset dev
```

Other presets: `release` (optimised, assertions still on at the fast level),
`bench` (assertions off, for measuring what they cost), `asan`, `tsan`.

**A change is not done until all four of `dev`, `release`, `bench` and `asan` pass.** The matrix
is not decoration: two defects in this repository were invisible in one configuration and fatal in
another — a dangling `span` into a destroyed temporary that `-O0` had not yet reused the stack for,
and a test asserting that a disabled assertion does not evaluate its condition, which the
assertions-off build reported as an unused declaration.

**What the matrix cannot see: the compiler.** It varies optimisation, assertion level and
sanitizers, all on one toolchain. On its very first run, CI found five call sites that were an
error under Linux Clang and silent under Apple Clang — a designated initialiser skipping a field,
where `-Wmissing-field-initializers` is implemented by one and not the other. Four local
configurations could not have caught it, and a verification story that omits what it cannot see is
the kind of overclaiming this project criticises elsewhere.

`.github/workflows/ci.yml` runs the same four presets, checks the committed traces still reproduce
byte-for-byte, and builds the viewer. Clang only for now, because this machine has no real GCC —
Apple Clang answers to `g++` — and a GCC job would be a configuration nobody had verified before
committing it.

## Documents

- `RESEARCH-DOSSIER.md` — how this problem was selected, and what was ruled out.
- `BUILD-GUIDE.md` — data sources, protocol specs, test targets, determinism hazards,
  learning path.
- `LUAN-GIAI-TIENG-VIET.md` — the same reasoning chain in Vietnamese.
- `docs/DESIGN.md` — mechanism choices, each with the real project and file that proves it works,
  plus why the two existing open-source MoldUDP64 libraries do not meet these requirements.
- `docs/STYLE.md` — house rules for comments, assertions, file size, aggregate defaults, README and
  commits, calibrated against measured comment and assertion density in Linux, SQLite, TigerBeetle,
  simdjson, quill and others.
- `viewer/README.md` — the one rule the viewer follows, and why it has no domain logic.
- `traces/` — recorded runs, committed as fixtures. `scripts/regenerate-traces.sh` then
  `git diff traces/` is a behavioural regression report.

# deterministic-feed-recovery

A seeded fault injector and a recovery library for exchange market-data feeds.

**Status: `dfr::core`, `dfr::wire`, `dfr::capture` and `dfr::chaos` are
implemented and tested. `dfr::recovery` is implemented and tested — arbitration, gap tracking,
retransmission and snapshot recovery, composed into one poll-driven client, with an
end-to-end oracle over both synthetic and real captures. `dfr::venue` is in progress —
the publisher and the retransmit and snapshot facilities are done, OUCH order entry is
not.**

564 tests pass under four configurations — assertions at paranoid, fast and off,
and AddressSanitizer + UndefinedBehaviorSanitizer — all with warnings as errors.

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
   can be tested against something that behaves like an exchange rather than a stub.

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

## Documents

- `RESEARCH-DOSSIER.md` — how this problem was selected, and what was ruled out.
- `BUILD-GUIDE.md` — data sources, protocol specs, test targets, determinism hazards,
  learning path.
- `LUAN-GIAI-TIENG-VIET.md` — the same reasoning chain in Vietnamese.
- `docs/DESIGN.md` — mechanism choices, each with the real project and file that proves it works,
  plus why the two existing open-source MoldUDP64 libraries do not meet these requirements.
- `docs/STYLE.md` — house rules for comments, assertions, README and commits, calibrated against
  measured comment and assertion density in Linux, SQLite, TigerBeetle, simdjson, quill and others.

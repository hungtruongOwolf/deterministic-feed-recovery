# deterministic-feed-recovery

A seeded fault injector and a recovery library for exchange market-data feeds.

**Status: planning. No code yet.** This repo currently holds the landscape research that
selected the problem.

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

## Documents

- `RESEARCH-DOSSIER.md` — how this problem was selected, and what was ruled out.
- `BUILD-GUIDE.md` — data sources, protocol specs, test targets, determinism hazards,
  learning path.
- `LUAN-GIAI-TIENG-VIET.md` — the same reasoning chain in Vietnamese.
- `docs/DESIGN.md` — mechanism choices, each with the real project and file that proves it works,
  plus why the two existing open-source MoldUDP64 libraries do not meet these requirements.
- `docs/STYLE.md` — house rules for comments, assertions, README and commits, calibrated against
  measured comment and assertion density in Linux, SQLite, TigerBeetle, simdjson, quill and others.

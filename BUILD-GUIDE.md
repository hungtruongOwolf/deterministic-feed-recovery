# Build Guide: Data sources, test targets, and learning path

Researched 2026-07-29/30. Every URL and byte count below has been verified with `curl` / `gh api` /
direct byte analysis. Anywhere verification was not possible is noted explicitly.

---

## SECTION 1: TEST DATA SOURCES

### 1.1 Start here: IEX HIST

This is the best source, nothing else comes close, for 5 directly-verified reasons:

1. **It is real pcap wire format**: Ethernet + 802.1Q VLAN + IPv4 multicast + UDP + IEX-TP. Not
   a normalized CSV, not a reconstructed file format. **[BYTE-VERIFIED]**
2. **IEX-TP is nearly a twin of MoldUDP64**: a 40-byte header with `Session ID`,
   `First Message Sequence Number`, `Message Count`, `Stream Offset`, `Send Time`. Parsing 20,145
   packets → **0 broken sequence transitions, 0 stream-offset mismatches**. Ground truth, exactly
   correct. **[BYTE-VERIFIED]**
3. **It has a complete, freely readable recovery spec**: A/B line arbitration, Gap Fill over both UDP
   and TCP unicast with Request Range Blocks, Gap Fill Test Request/Response, heartbeat, session
   termination, plus a separate snapshot protocol (**SNAP**). This is the rarest thing in this entire
   field: a real recovery spec, with real data that matches it.
4. **Free, no key, no registration, no click-through ToS.** Updated T+1.
   **2,434 trading days, 2016-12-12 → 2026-07-28.**
5. **Range requests work** → never have to download 13 GB. A 4 MB prefix of the gzip stream
   decompresses to a valid ~17 MB pcapng. **[BYTE-VERIFIED]**

**Command to get the first test file:**

```bash
# 712 KB compressed → 3.4 MB pcap. Saturday 2017-08-26 = a weekend session:
# small, but structurally complete (real session ID, real sequence numbers, 13,220 heartbeats).
curl -L -o deep_20170826.pcap.gz \
 'https://www.googleapis.com/download/storage/v1/b/iex/o/data%2Ffeeds%2F20170826%2F20170826_IEXTP1_DEEP1.0.pcap.gz?generation=1503943426454167&alt=media'
gunzip deep_20170826.pcap   # → 3,405,890 bytes, classic libpcap (d4c3b2a1), µs, Ethernet
```

This file is **classic pcap** (easier to parse than pcapng): a deliberate choice. Then, for a real
trading day:

```bash
# Always re-resolve the link: the ?generation= token is the object's version.
curl -s 'https://iextrading.com/api/1.0/hist?date=20191224' | python3 -m json.tool
# 20191224 = a half-day session: DEEP 171,929,929 B (smallest regular weekday), TOPS 160.6 MB
```

**Three traps to know before writing the parser:**

| Trap | Detail |
|---|---|
| **Switches from pcap to pcapng partway through** | `20170615` = classic pcap (`d4c3b2a1`). From `20170620` onward = **pcapng** (`0a0d0d0a`). You need both readers. **[BYTE-VERIFIED]** |
| **Has an 802.1Q VLAN tag** | VLAN 1013 in the 2017 file. If you hardcode `ethertype @ offset 12 == 0x0800`, parsing yields **zero**. **[BYTE-VERIFIED]** |
| **HIST pcap contains only ONE multicast group** | `233.215.21.4:10378` for DEEP. IEX-TP *does define* an A/B line, but the archive only captures one line. **You cannot test A/B arbitration with IEX HIST data.** Use B3 for that. |

### 1.2 Two supplementary sources you must have

**a) NASDAQ's real MoldUDP64 pcap**: solves the biggest gap in the plan:

`Open-Markets-Initiative/omi-data-pcaps` → `nasdaq/NasdaqEquities/TotalViewItch.v5.0.zst`
(17,728,924 B → 71,697,245 B). This is a real NASDAQ TotalView-ITCH 5.0 **multicast MoldUDP64**
pcap: session `000010059B`, seq 10,312,965 → 10,740,083, **399,999 seamless transitions / 0
breaks**, 627,466 packets on `233.54.12.111:26477`, VLAN 141. **[BYTE-VERIFIED]**

**b) A real A/B pair**: the only source found anywhere in the world:

B3 (the Brazilian exchange): `MBO_EQT_Incremental_FeedA.zip` + `FeedB.zip` (1,422,257,073 +
1,421,936,528 B, free, no key required): **a redundant A/B capture pair of the same session**,
plus `MBO_EQT_SnapshotRecovery.zip` (204,135,545 B) separately. This is the only thing that lets
you test A/B arbitration with real data.

### 1.3 ⚠️ IMPORTANT FINDING: the free NASDAQ data has NO transport layer

This is a finding that changes the plan, and it holds true for **every file, including the newest
2026 ones**.

Byte-dissecting three files:

```
01302019.NASDAQ_ITCH50.gz   → 00 0c | 53 ... 4f    (len=12, 'S' System Event, code 'O')
                               00 27 | 52 ...       (len=39, 'R' Stock Directory)
S061226-v50.txt.gz (17.9GB) → 00 0c | 53 ... 4f    identical framing
itch50_05_18.gz    (16.1GB) → 00 0c | 53 ... 4f    identical framing
```

Decoding 24 consecutive messages, `len/type` matches the ITCH 5.0 struct sizes exactly
(`S`=12, `R`=39, `A`=36, `F`=40, `U`=35, `X`=23, `E`=31, `P`=44, `D`=19). **[BYTE-VERIFIED]**

**This is NASDAQ's "BinaryFILE" format: just a 2-byte big-endian length + a raw ITCH message. NO
MoldUDP64 header, NO session ID, NO packet sequence number, NO UDP, NO pcap.** The entire transport
layer has been stripped away.

Confirmed directly against NASDAQ's own spec
(`nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/binaryfile.pdf`, 84,384 B):

> *"BinaryFILE is a very simple file format used to deliver a set of sequenced messages inside a
> static file... It is intended as an **off-line companion** for real-time message delivery
> protocols like SoupBinTCP and MoldUDP64."*
> *"A **message of length zero is used to indicate the end of the session.**"*

**Consequence:** if you only use `emi.nasdaq.com`, you will have to **synthesize the entire
transport layer yourself**: invent your own session ID, pack messages into MoldUDP64 datagrams
yourself, generate your own heartbeats, then inject gaps into that self-made framing. **Your gaps
would be testing your own generator, not real wire behavior.** Use the OMI MoldUDP64 pcap from
§1.2a for real framing; use emi.nasdaq.com for realistic volume and message mix.

Also note: every `.md5sum` file in that directory returns **404** (IIS is missing the MIME
handler). Don't build checksum verification that depends on them.

### 1.4 Crypto: a free and unlimited gap-recovery corpus

Because crypto exchanges emit monotonic sequence numbers and **publish their resync procedure in
writing**, they give you an unlimited, free, ground-truth gap-recovery test corpus. Binance has a
"how to manage a local order book" procedure that is, in essence, a gap-recovery algorithm, with
`U`/`u` fields. Coinbase Advanced has `sequence_num`. Use as a supplementary source, not the
primary one (JSON/WebSocket, so it doesn't teach anything about binary multicast).

---

## SECTION 2: SPECS: WHERE TO GET THEM

All the links below require **no registration** and have been verified live:

| Spec | URL |
|---|---|
| MoldUDP64 V1.00 (6 pages, complete) | `nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf` |
| SoupBinTCP 3.00 (9 pages) | `.../dataproducts/soupbintcp.pdf` |
| **Nasdaq GLIMPSE 5.0 (32 pages)** | `.../dataproducts/nqglimpsespecification.pdf` ⚠️ watch the filename: `glimpse.pdf`, `nqglimpse.pdf` both 404 |
| TotalView-ITCH 5.0 | `.../dataproducts/nqtvitchspecification.pdf` |
| OUCH 4.2 | `.../TradingProducts/ouch4.2.pdf` |
| BinaryFILE | `.../dataproducts/binaryfile.pdf` |
| IEX-TP 1.25 (15 pages) | ⚠️ the live URL now just returns a 1-page "moved" stub; the real document is at `web.archive.org/web/2020/https://iextrading.com/docs/IEX%20Transport%20Specification.pdf` |
| IEX DEEP 1.x (44 pages) | same pattern; has real A/B/C addresses and retransmit limits |
| UTP Data Feed Services v4.1 | `utpplan.com/DOC/UTPBinaryOutputSpec.pdf` |
| ⭐ **SIAC Retransmission and Snapshot User Guide v1.8** | `cdn.opraplan.com/documents/SIAC_Retransmission_User_Guide.pdf`: **the best public documentation of a real production retransmit facility**: real rate limits, response codes, sequence-reset semantics |
| OPRA Common IP Multicast Distribution Network | `cdn.opraplan.com/documents/OPRA_Common_IP_Multicast_Distribution_Network.pdf`: A/B stream architecture, 156 lines → 312 groups |
| RFC 3208 (PGM) | `rfc-editor.org/rfc/rfc3208.txt` |

---

## SECTION 3: LIST OF TEST TARGETS

This is the single most important deliverable of this research phase. After sweeping **232
market-data repos** (deduplicated from 30+ repo queries + 8 code-identifier queries), then
**reading the actual source** rather than trusting the name: only **9 implementations across all
of GitHub** genuinely have a MoldUDP64 gap state machine **that also issues a retransmission
request**.

### Tier 1: verified by reading source

| Target | ★ | Push | License | Evidence read |
|---|---|---|---|---|
| **paritytrading/nassau** | 106 | 2026-07-25 | Apache-2.0 | `MoldUDP64Client.java:269-284`, `nextExpectedSequenceNumber`, `BACKFILL` vs `GAP_FILL` states, `requestUntilSequenceNumber`, cold start (`==0`). 21 test files, CI, CHANGELOG. **This is the reference semantics.** |
| **penberg/helix** | 122 | 2017-10-18 | BSD-2 | `moldudp64.hh:93-136`, drops stale/dup, enters `gap_fill`, `retransmit_request()` emits `htobe64(expected_seq_no)`. Weak: 1 test file, does not compile. |
| **leonardorufino/ll-hft** | 1 | 2026-05-20 | MIT | `receiver_session.hpp` (838 lines): power-of-two reorder buffer, `try_pop(expected_seq)` validates `slot._first_sequence_number`, separate `RetransmissionController` with a deadline timer. **The most complete recovery design in any recent repo.** 301 files / 55 test files; 90 commits spread across **76 distinct days** (the cadence of a real person). |
| **ohmp7/MarketDataFeedHandler** | 3 | 2026-01-12 | none | `moldudp64.cpp:54-75`, clear `Backfill (cold start)` / `Gapfill` / recovery-window update, has a `kSynchronized` sentinel + `std::max`. A faithful C++ port of nassau. **0 test files.** |
| **anminfang-tamu/astra-feed-engine** | 0 | 2026-07-30 | none | `MoldUdpDecoder.cpp:98-121`, `ChannelHealth::GapDetected` plus its own `heartbeat_next_seq_high_watermark`, so heartbeats push the watermark forward instead of being counted as a gap. **That is a correct and non-obvious protocol detail** that an LLM rarely comes up with on its own. |
| **Rfkir/HFT_CPP** | 0 | 2025-11-27 | none | `moldUDP64_rxtx.cpp:234-288`, two gap paths, request clamped with `std::min<uint64_t>(gap, 60000)` (matches the protocol's real cap). Comments in Turkish: not LLM English. |
| **JacobNickerson/money-matcher** | 1 | 2026-04-23 | none | `receiverhandler.rs` (436 lines): its own `retransmission_socket`/`addr`, `gap_buffer: BTreeMap<u64, MarketEvent>`. |
| **an-thony350/ITCH-Feed-Handler-and-Order-Book** | 3 | 2026-07-29 | MIT | `rtl/mold_seq_guard.sv` (138 lines) + `mold_deframe.sv` (862 lines), **plus** `tb/test_mold_seq_guard.py` cocotb (274 lines) and 2 xsim testbenches (920/216 lines), plus `docs/moldudp64_seq_handling.md`. **The strongest recent target.** README: 0 emoji, 0 badges, 52 lines of real numbers. |
| **jamisonrobey/nasdaq-moldudp64-feed-sim** | 0 | 2026-07-12 | MIT | The **server/rewinder** side: see §3.3 |

Beyond MoldUDP64: **epam/java-cme-mdp3-handler** (82★, LGPL-3.0, 280 files / **129 test files**,
owned by EPAM the company) is the best CME MDP3 target. **spiretrading/nexus** (4,298 files, 249
test files, 10 years of history) is excellent engineering but its `MoldUdp64Client.hpp` is only
128 lines, **has no gap token at all**: it only does framing, wrong layer.

### 3.1 ⭐ `coryan/jaybeams`: a free negative control

`mold_udp_channel.cpp:71-104` detects `sequence_number != expected_sequence_number_` and then
**logs and continues**. The source comment says it plainly: *"since we are not dealing with gaps,
or message reordering."* Apache-2.0, 432 files, author is a Google engineer.

This is exactly the canary you need, and it already exists, honestly self-admitted, with nobody
having to call it out. **Your harness MUST flag jaybeams, or your harness is wrong.** Put it into
CI as a must-fail fixture on day one. It also gives you a graceful way to publish a "did not
recover" result: the author already documented the scope limit themselves.

Secondary controls: `evanap003300/orderbook` (*"here we log and skip"*), `ArkaKhorchidian`
(`detect_gap()` is empty, no recovery).

### 3.2 ⚠️ Deadly trap: two "independent implementations" are actually the same code

`nixiz/itch-bist-parser`: the `moldudp64.hh` file is **149 lines, versus 147 lines in
`penberg/helix`, with nearly identical identifiers at nearly the same line numbers**. This is a
fork, not an independent implementation.

This is the most dangerous trap for a consensus oracle. Two forks in a 3-vote ballot produce a
**2-1 majority that is really just the same upstream bug**, and it will make the third (correct)
implementation look wrong. Before admitting any implementation into a consensus set, you must diff
it against every other member and require real independence. `helix` and `nixiz` count as **one
vote**.

**The independently verified consensus set:** `nassau` (Java) + `helix`-or-`nixiz` (one vote, C++)
+ `ll-hft` (C++) + `astra-feed-engine` (C++) + your own `std::map` referee = **4 real independent
votes plus one model**: enough for a 3-2 verdict without needing an arbiter.

### 3.3 Someone has already built a fault-injection *source* for you

`jamisonrobey/nasdaq-moldudp64-feed-sim` (MIT, 56 files, 17 test files, 2 CI workflows) is a
MoldUDP64 **server** with `imr/mold/retransmission/{feed,feed_pool}` and
`downstream/{feed,heartbeat,pacer}`. That is, a transmitter with a rewinder and a retransmission
responder.

This solves half the problem: **testing gap *recovery* is impossible if nothing answers a
retransmit request.** Nobody recovers from a gap if nobody responds. Evaluate this repo before
writing your own transmitter.

Two more gifts: `joaquinbejar/itch-rs` has **13 fuzz targets**, and `bbalouki/itchcpp` has
`fuzz/moldudp64_fuzzer.cpp`. Harvest their corpora as seeds regardless of whether you use the test
repo itself.

### 3.4 ⚠️ The 2026 repo wave: quantified

232 market-data repos after deduplication. **156 (67%) were created in the last 19 months**: 51 in
2025, 105 in the first 7 months of 2026, versus a 2020-2024 baseline of ~7/year. That's roughly a
**25× increase**.

Of those 156 repos: **82% have 0 stars**, 80% have no license, 58% are C++, **21% have only 1
commit**, **49% have all their commits on a single day**, 71% have ≤3 distinct commit days, **58%
were created and last pushed on the same day**. Median lifespan: **1 day**. Seven repos are
**completely empty git repos**.

**Usable fraction of the whole wave: ~12-15 out of 156 (~9%).**

Important: **151 different owners across 156 repos**, this is not a bot farm, it's ~150
individuals each creating a one-off portfolio artifact. That's a sign of an LLM-assisted resume,
not spam.

**One specific sub-pattern worth naming in your README:** 55 recent repos share the name
`MarketDataFeedHandler`, three of them literally named `qode-c-assignment-market-data-feed-handler`
/ `C-_Assignment_Qode_MarketDataFeedHandler` / `Market-Data-Feed-Handler-Assignment`, and **seven
of them empty**. This is *one* recruiting take-home assignment cloned ~55 times. **Exclude all of
them**: testing it would look like farming volume.

Best illustration of why matching file names is useless: **`ray-27/ITCH-feedhandler` has exactly 1
commit, no README, no license, but contains a `gap_fill_client.hpp` with a retransmit API that
looks very plausible.**

### 3.5 No gap handling despite what the name suggests

`th2-net/th2-codec-moldudp64`, `bbalouki/itchcpp` (transport does nothing, despite having a mold
fuzzer), `sqfzy/ephemeral` (208 lines, 0 tokens), `csinitiative/fhce`, `vladium/vrt`,
`ikravets/ev`, `Mister-Meeseeks/moldudp-unwrap`,
`Francklin9999/low-latency-trading-pipeline` (19-line header).

**Overclaimed name: no such source exists:** `Kirill-Katz/itch-ingestion-engine` and
`Yashwanth-1412/QuantLink` both show up in a MoldUDP64 search; a recursive tree walk over the whole
repo finds **0 files** matching `mold|udp|seq|recv`. The claim exists only in README text.

---

## SECTION 4: THE MOST IMPORTANT TECHNICAL LESSON: THE GLIMPSE RACE CONDITION

This is the heart of the design, and what the fault injector should attack.

```
t0   You detect an unrecoverable gap (or you are cold-starting).
t1   You open a SoupBinTCP connection to Glimpse and log in with seq=1.
     ---- MEANWHILE, multicast ITCH keeps running at full rate ----
t2   Glimpse starts to "spin". This is a snapshot of state AT THE MOMENT of the login request (t1).
t3   ... spinning continues. NASDAQ has ~8-11k symbols, a full displayable book on the order of
     10^5-10^6 orders waiting. The spin is exactly that many Add Orders over ONE TCP connection.
     In practice: tens of seconds.
t4   You receive "G" with Sequence Number = N (the ITCH sequence at t1).
t5   You now have to be at exactly the right position to process ITCH message number N.
```

**The race is here: `N` is determined at `t2`, but you only learn it at `t4`.** Everything
multicast emits during `[t2, t4]`, potentially tens of seconds of full-rate data, hundreds of
thousands of messages, is both **necessary** and **not yet usable**, because you cannot apply it
before the snapshot load finishes, and you cannot know where to start before the snapshot ends.

**The one correct architecture:**

```
Upon entering state RECOVERING_VIA_SNAPSHOT:
  1. KEEP the joined multicast socket and KEEP draining it. Do not close it.
     Buffer every message with its sequence number into a bounded ring.
     (If you close the socket to "save work", you have turned a recoverable gap into an
      unrecoverable one, because the re-request server's retention window is finite and
      you will fall off its tail.)
  2. In parallel, run the Glimpse spin into a SHADOW book. Never touch the live book.
  3. Upon receiving "G" with sequence N:
       - discard buffered messages with seq <  N
       - apply buffered messages with seq >= N to the shadow book, in order
       - if the lowest seq still held in the buffer > N  ==>  RECOVERY FAILED.
         You buffered too late or too little. Escalate: re-request [N, buffer_low)
         from the re-request server, or restart the whole snapshot.
       - once caught up to live, swap shadow -> live atomically
  4. Only at this point do you publish.
```

**Why this is exactly the place for the fault injector to attack**: every failure mode is
**silent** and produces a book that **looks plausible but is wrong**:

| Fault injected | What a correct client must do | Symptom of an incorrect client |
|---|---|---|
| Multicast buffer smaller than (spin duration × peak rate) | Detect `buffer_low > N`, escalate | Silently applies from `buffer_low`, **permanently losing** `[N, buffer_low)`. Book has phantom orders forever. |
| Slow spin (throttle Glimpse's TCP down to 1 Mbps) | Bounded buffer fills → detected → escalate or restart | OOM, or the ring wraps and overwrites without anyone noticing |

---

## SECTION 5: FAULT INJECTOR DESIGN

**Principle number one: model by BURST, not by probability.** An injector like
`drop_probability=0.001` tests almost nothing real.

Correct parameterization:

```
BurstLoss   { start_offset, duration_ms ∈ [1,10], rate_multiplier ∈ [3,10],
              lines: {A}|{B}|{A,B}|all, channels: one|correlated_subset|all }
Reorder     { seq, displacement, delay_ns ∈ [1µs, 10ms] }
Duplicate   { seq, whole | straddling_expected }
LineDeath   { line, mode: silent | stale_seq | heartbeat_only | lagging(growing) }
Epoch       { session_change | seq_rollover_2^32 | reset_to_1 ± marker
              | sequence_discontinuity }
Heartbeat   { stop_data_keep_hb | stop_both | hb_next_expected_ahead | freeze_hb_seq }
Facility    { partial_response(n) | no_response | reject(code)
              | ignore_second_concurrent | terminate_on_malformed
              | rate_limit_then_dos(60s) }
Snapshot    { slow_spin | splice_behind_buffer | splice_ahead | transact_time_mismatch
              | join_mid_loop | totnumreports_grows | session_changes_mid_spin
              | gap_within_buffered_stream | connection_drops_mid_spin }
Event       { drop_end_of_event | empty_end_of_event | split_across_n_packets
              | splice_mid_event }
Framing     { exceed_mtu | drop_fragment | unknown_msg_type | msg_longer_than_expected
              | split_tcp_at_every_byte | coalesce_many_packets_per_recv }
Consumer    { stall_ns, egress_backpressure }
StateReset  { quote_wipeout | per_security_wipe_no_message | channel_reset }
```

**Determinism:** seed everything from a single PRNG seed; make the injector a deterministic
function of `(seed, packet_index)` so a failing case replays exactly. Draw time from an injected
clock, so backoff timers can be tested without waiting for real time.

**Highest-value injectors, in order:** `Facility.reject` (verify there is no retry storm and no
self-DoS) → `Snapshot` (all 8 modes) → `BurstLoss` correlated across every line and channel →
`LineDeath.silent` on one line → `Epoch` → `Event` (event atomicity).

**And inject faults into the recovery path itself.** The root cause of the 2013-08-22 NASDAQ
incident was a failover path with a latent bug **that had never actually run**. Your recovery code
is the least-tested code you have, and it only runs when everything has already gone wrong. Use
IEX's **Gap Fill Test Request** idea, generalized: continuously run the recovery path in production
against a synthetic gap, so it is never cold.

---

## SECTION 6: LEARNING PATH (trimmed)

**The most important reordering:** this project **trades nanoseconds for correctness**, so the
memory model is a *hygiene* requirement (a correct SPSC handoff, a seqlock), **not** the
differentiator. The differentiator is protocol semantics, ordering/recovery theory, and
deterministic fault injection.

Total ~60 hours, learned just-in-time, not front-loaded:

| # | Area | Hours | Why it sits here |
|---|---|---|---|
| 1 | Trading domain: **only a thin slice** | 10 | You can't design the recovery layer before knowing MoldUDP64 sequencing and what Glimpse does. Blocks everything. |
| 2 | DST foundations (2 talks) | 4 | Decides the **architecture** of all 3 components. Watch in the first week. |
| 3 | Distributed systems: selective | 8 | Provides the vocabulary the write-up needs to be credible. |
| 4 | Linux multicast networking | 10 | Needed for the receiver + local test harness. |
| 5 | Memory model / lock-free | 12 | Needed, but **less** than it feels at first. |
| 6 | C++20 coroutines | 10 | **Can be deferred.** See the warning below. |

### Top 5 must-watch (4h13m total)

| # | Talk | YouTube ID | Length | Why |
|---|---|---|---|---|
| 1 | **Will Wilson: Testing Distributed Systems w/ Deterministic Simulation** | `4fFDFbi3toc` | 40:20 | Your project's manifesto, from FoundationDB. Watch first. |
| 2 | **TigerBeetle: How to write your own Deterministic Simulator** | `JoYjji1DZCE` | 71:22 | **The single most actionable resource on this whole list**, they built exactly your Component 3. |
| 3 | **Carl Cook: When a Microsecond Is an Eternity** (CppCon 2017) | `NH1Tta7purM` | 60:07 | A talk for domain context. |
| 4 | **Gil Tene: How NOT to Measure Latency** | `lJ8ydIuPFeU` | 42:59 | Vaccine against the #1 way published numbers get broken. Watch **before** publishing anything. |
| 5 | **David Gross: When Nanoseconds Matter** (CppCon 2024) | `sX2nF1fW7kI` | 88:51 | How an engineer at a real trading firm talks about this. |

### Reading, in priority order

1. **Databento microstructure guide**: `databento.com/microstructure` + `/market-data-feeds`.
   The best free, modern, and directly on-topic resource: MBP vs MBO, L1/L2/L3, feed types.
   **Higher ROI than Harris for this project.** (4h)
2. **The specs themselves, in Section 2**: this is the real learning material. Your project *is*
   literally an implementation of these semantics. Read the sequencing and recovery sections until
   you know them by heart.
3. **Kleppmann, Cambridge "Concurrent and Distributed Systems"**:
   `cl.cam.ac.uk/teaching/2223/ConcDisSys/dist-sys-notes.pdf`, **only Ch 2, 3, 4**. Ch 4
   "Broadcast protocols and logical time" *is* literally the theory of a multicast feed with
   gap-fill and A/B arbitration. Ch 2 gives you the vocabulary for a fault model. (6h)
4. **Phi accrual failure detector**:
   `dspace.jaist.ac.jp/dspace/bitstream/10119/4784/1/IS-RR-2004-010.pdf`, §2-4. Exactly the
   problem of a **dead line vs a slow line**. Naive timeouts are the obviously wrong answer and
   this paper explains why. 17 pages. (1.5h)
5. **Lamport, "Time, Clocks, and the Ordering of Events"**: 8 pages, and it is the foundation of
   "sequence number *is* logical time". (1h)
6. **Martin Fowler, LMAX architecture**: `martinfowler.com/articles/lmax.html`. The LMAX
   architecture *is* your pattern: a sequenced single-writer ring, an event-sourced log,
   deterministic single-threaded business logic **so it can be replayed**. Same idea as DST, from a
   real exchange. (2h)
7. **Preshing**: `preshing.com`, read in this order: `an-introduction-to-lock-free-programming` →
   `memory-reordering-caught-in-the-act` (**the "aha" post**) → `memory-barriers-are-like-source-control-operations`
   → `acquire-and-release-semantics` → `the-synchronizes-with-relation`. **The best pedagogy in the
   field, and free.** (4h)
8. **Hans Boehm: "Using weakly ordered C++ atomics correctly"** (CppCon 2016, `M15UKpNlpeM`,
   63:25). Highest value per minute. **Prioritize over Sutter if short on time**, and it directly
   backs up your TSan argument.
9. **Russ Cox, memory models**: `research.swtch.com/hwmm` then `/plmm`. The best explanation
   anywhere of x86-TSO vs ARM/POWER. **Backs the pitch**: "TSan on x86 hides a bug that will show
   up on Graviton."
10. **Zeller & Hildebrandt, "Simplifying and Isolating Failure-Inducing Input"** (TSE 2002):
    `st.cs.uni-saarland.de/papers/tse2002/tse2002.pdf`. The classic **ddmin** paper, and it is the
    **single biggest multiplier of your artifact's value**. "Reproducible from seed 4711" is good.
    "Reduced to a minimal 3-packet counterexample, and here is the invariant it violates" is *much*
    stronger.
11. **Jepsen analyses**: `jepsen.io/analyses`. Read 1-2 of them **to learn how to write, not the
    technique**. This is the gold standard for "here is the bug, here is how to reproduce it, here
    is the limit of what I tested".

### ⚠️ Cut, with reasons

- **MIT 6.824** (now **6.5840**): **mostly the wrong direction.** Its backbone is consensus and
  replication (MapReduce, GFS, Paxos, Raft ×2, ZooKeeper, Spanner, BFT, Bitcoin). **No lecture on
  failure detection, none on Lamport clocks.** Your project **has no consensus problem**. Only
  watch **2 of the 22 lectures**: LEC 8 (Consistency & Linearizability) and LEC 15 (IronFleet).
  **Skip the labs entirely.**
- **Raft**: don't implement it; there is no leader election in a recovery layer. Only read **§5.3
  (log matching)** and **§7 (snapshotting)** as design analogies.
- **Bouchaud et al "Trades, Quotes and Prices"**: cut entirely. For the quant *researcher*.
- **Almgren**: cut. Optimal execution, wrong field.
- **Harris "Trading and Exchanges"**: **only Ch 4, 5, 6** (~120 pages).
- **Daniel Anderson, `atomic<shared_ptr>`** (CppCon 2024): **cut.** It solves *memory
  reclamation*. Your design uses a bounded ring buffer and an arena/slab, **there should be no
  `shared_ptr` anywhere near the hot path.**
- **Fedor Pikus, "The Art of Writing Efficient Programs"**: cut (a *performance* book; this is not
  a performance project).
- **McKenney "Is Parallel Programming Hard"**: **only Ch 15 (Memory Ordering) and Ch 11
  (Validation)**. Don't read all 700+ pages.
- **Elle**: cut the *technique* (it finds cycles in a transaction dependency graph; you have no
  transactions). Read the Jepsen *reports*, not Elle.
- **`memory_order_consume`**: skip. Compilers promote it to acquire.

### ⚠️ Warning about coroutines before investing 10 hours

The deterministic **scheduler itself is the easy part**: a priority queue over simulated time plus
a seeded PRNG. Coroutines only make *user code* easier to write. Evidence: **TigerBeetle's VOPR
does not use coroutines** (explicit state machines + a deterministic event loop), and FoundationDB's
Flow uses a compiled CPS/actor model.

The argument "C++20 coroutines remove the barrier that made Flow its own dialect" is correct and
**is exactly the differentiator**. But: **build the core simulator with an explicit event queue
first and get it passing tests, and only reach for coroutines once the ergonomics of a hand-written
state machine actually start to hurt.** Don't let coroutine theory become an excuse to stall.

When you need it: **Lewis Baker** at `lewissbaker.github.io`, in this order `coroutine-theory` →
`understanding-operator-co-await` → `understanding-the-promise-type` →
`understanding_symmetric_transfer` (**the most important post for you**: about avoiding stack
overflow when resuming a chain, exactly what happens when the simulator runs millions of events).
⚠️ This series is **unfinished**: don't expect it to cover a whole scheduler.

---

## SECTION 7: LINUX MULTICAST: TWO THINGS THAT WILL BITE YOU

Both are poorly documented in the OSS world:

- **`IP_MULTICAST_ALL`**: by default, a socket receives traffic for **every group joined on the
  host**, not just the group it joined. Must be set to 0, or you get a silent cross-channel leak.
  This is a real bug in most open-source multicast receivers.
- **`SO_REUSEADDR` vs `SO_REUSEPORT`**: `SO_REUSEADDR` is what lets multiple sockets bind the same
  group/port for fan-out. `SO_REUSEPORT` **load-balances**, which is the opposite of what a feed
  reader wants. Correctly stating this difference in writing is a real differentiator.

Docs: `man7.org/linux/man-pages/man7/ip.7.html` (especially `IP_ADD_SOURCE_MEMBERSHIP`, SSM,
commonly used by real exchange feeds), `docs.kernel.org/networking/snmp_counter.html` (authority
on `UdpInErrors`/`RcvbufErrors`, i.e. what `netstat -su` actually means),
`man8/tc-netem.8.html` + `man8/ip-netns.8.html` + `man4/veth.4.html` (**this is the test harness**:
inject loss/latency/reorder/duplicate without hardware),
`blog.cloudflare.com/how-to-receive-a-million-packets/`, `rigtorp.se/sockets/`.

**Plan around this now:** AWS **does not support multicast on a standard VPC** (needs a Transit
Gateway multicast domain), and IGMP behaves differently under veth/netns than on a real switch.
Given the cloud-only constraint, **all local testing must be netns/veth or loopback**, and IGMP
snooping/querier, the #1 operational cause of a "feed just stops naturally", is something you will
have to reason about rather than reproduce. **Write this down explicitly in the README.** A
declared limitation is an asset; a limitation someone else discovers is a wound.

---

## SECTION 8: CRAFT: HOW AN ARTIFACT GETS RECOGNIZED

### Repo: what actually builds credibility

Pulled the full metadata and README of `rigtorp/hiccups` (136★, **1 CI job, 0 test dir**),
`max0x7ba/atomic_queue` (1,880★), `odygrd/quill` (2,981★). **Main finding: credibility does not
come from badge count or repo size. It comes from disclosing method and being honest about
alternatives.**

`hiccups` (an artifact the dossier says could be pulled from the CppCon channel) does these things:

- **Explains the method in prose BEFORE giving any number.**
- **Publishes how the threshold was derived**: *"the threshold is computed as 8 times the smallest
  difference between two consecutive timestamps across 10,000 runs."*
- **Credits prior art**: David Riddoch's `sysjitter`.
- **Names a solution BETTER than itself and says why**: Linux osnoise tracer *"also measures
  system jitter, and additionally shows you the sources of the jitter."* → **Pointing the reader to
  something better than your own tool is the strongest credibility signal there is.**
- **Reports p99 / p99.9 / max: never mean.**
- **No adjectives.** The description is *"Measures the system induced jitter…"*: what it does, not
  how fast it is.

`quill` is the model for reporting percentiles: it has a **"System Configuration"** section BEFORE
any numbers: OS, CPU + exact clock, compiler version, and **pastes the raw `/proc/cmdline`**
(`isolcpus=1-5 nohz_full=1-5 mitigations=off processor.max_cstate=1 intel_pstate=disable`).
Benchmark code lives in a **separate public repo** so anyone can rerun it. Tables for
50/75/90/95/99/99.9: **no mean anywhere**. And the **decisive move: quill publishes the tables
where it loses too**, XTR and NanoLog beat it at p50-p99; quill only wins at p99.9. Then its
"Verdict" section **recommends the competitor** for other use cases.

**Note on AI:** `quill`'s tree has a `CLAUDE.md` file, and it is one of the most trusted repos in
the field. **Using AI is not the disqualifier: unreviewed AI output is the disqualifier.** The tell
in the case where a CTO called someone out was **unreviewed breadth**: many topics discussed very
confidently, no measurements, no failure modes.

**Checklist ranked by signal value:** (1) state the method next to every number, machine, command,
statistic, run count; (2) percentiles, never mean; (3) **one table or result you don't win**,
naming the competitor; (4) failure modes and caveats stated plainly; (5) name a better solution if
one exists; (6) credit prior art; (7) a harness a stranger can run; (8) accurate description,
**no adjectives**; (9) CI that actually runs tests + sanitizers; (10) has a license; (11) tests in
a recognizable framework; (12) badges: **the weakest signal**.

### Writing: a template drawn from `rigtorp.se/ringbuffer/`

That post is **1,522 words, 10 code blocks, 0 charts**. First lesson: the model post is **short**.

Its moves, in order: (1) a one-sentence scope statement; (2) the headline is a before-to-after
delta against a named baseline: *"increased throughput from 5.5M to 112M items/s, beating both
Boost and Folly"*; (3) link the production implementation immediately; (4) anchor to a real
application (NIC ring, io_uring CQ); (5) build up from a naive version, show full code; (6)
**explain the benchmark parameters AND state explicitly what you chose NOT to measure (at the
decision point, not in a footnote)**; (7) an exact reproduction recipe: file name, compile line,
CPU, thread placement, run command; (8) **paste the raw `perf stat` counters**: **the number is
explained by mechanism, not just asserted** (the single most credible move in the post); (9)
invites verification; (10) a "Further optimizations" section, an honest scope boundary.

**The transfer principle:** every number comes with (a) the command that produced it, (b) the
machine it ran on, (c) the mechanism that explains it, (d) a statement of where it does not
generalize.

**Your adaptation.** Your project is a *correctness* project, so the equivalent of `perf stat` is
the **reduced counterexample plus the violated invariant**:

> **Claim** (verifiable, one sentence), **one-command repo with seed**, **minimal fault
> schedule** (delta-debugged from the raw trace), **violated invariant**, **mechanism**, why that
> handler's gap logic fails, **scope limits**: which version, which config, what is *not* tested,
> **prior art and alternatives**, named fairly.

And: declare the cloud-VM constraint **before anyone asks**, software timestamps only, no PMC, no
hardware timestamping.

### Publication channels: real CFP status as of 2026-07-29

| Channel | Status |
|---|---|
| **CppCon lightning talk** | ⭐ **The nearest, best opportunity.** Submit via the form at `cppcon.org/lightning-talks-and-lightning-challenge/`. **5 minutes.** No announced deadline. And importantly: *"lightning talk sessions are open to anyone, regardless of whether they have a conference ticket, **even if you want to speak**!"* |
| CppCon 2026 full session | **CLOSED** (ran 2026-04-17 to 2026-05-17). Conference 2026-09-12 to 09-18, Aurora CO, on-site only. |
| **P99 CONF 2026** | 2026-10-21 to 10-22, online, **free**. CFP **closed** (2026-01-05 to 2026-05-29). Format: **15-20 minute pre-recorded**. Has a **"Measurement (tools, tracing, benchmarking, observability)"** track (a direct match. **The 2027 CFP might open ~2027-01**, set a reminder.) |
| **Show HN** | Always open. ⚠️ **A blog post alone CANNOT be a Show HN**: *"a blog post... isn't something you can try, so it can't be a Show HN."* A **tool** does qualify. |
| Meeting C++ / ACCU / Core C++ / C++ on Sea | Site is live, ⚠️ could not extract a current CFP deadline. |

**Immediate deadline:** CppCon 2026 volunteer registration **closes 2026-08-01, two days from
now.** Volunteering is a cheap, legitimate way to be in the building with those sponsors this
September.

### Anti-patterns, with evidence

| Anti-pattern | Evidence |
|---|---|
| **"Ultra-low-latency" in the title** | The HN post that got called out by an HFT firm's CTO was titled **exactly** *"Ultra-Low-Latency Trading System"* (story 46384415, 2025-12-25). Contrast: `hiccups`, *"Measures the system induced jitter…"*, no adjectives. |
| **A repo that reads as LLM-written** | Verbatim comment 46387677 from `raviolo`: *"I'm the CTO of an HFT firm. My take: the repo (and probably the author's comments too) is LLM-generated... **Either way, it saved you the trouble of writing 'generate low-latency trading system' as a prompt.**"* → That last line is the worst thing that can be said about a portfolio artifact: **it reduces the effort to a single prompt.** |
| **Unmeasured / made-up numbers** | The antidote is quill's raw `/proc/cmdline` and atomic_queue's Methodology section. **If you can't paste the command and the machine, don't publish the number.** |
| **TPS from a loop that never touches the data structure** | The defense is rigtorp's `perf stat` (proves **mechanism**) plus publishing the tables where you lose. |
| **Mean instead of percentile** | Neither `hiccups` nor `quill` **reports mean anywhere.** |
| **50 ALL-CAPS `*_REPORT.md` files** | `hiccups`'s entire tree has 7 entries. All three credible repos: **0 status-report markdown files.** |

---

## SECTION 9: BUILD ORDER (adjusted for new findings)

1. **Day 1: make `coryan/jaybeams` a must-fail CI fixture**, before any real target.
   It validates your own harness, for free.
2. **Download the first IEX HIST file** (§1.1), write a pcap+pcapng reader, handle the VLAN tag,
   parse IEX-TP, confirm 0 sequence breaks on clean data. This is your "clean" baseline.
3. **Get the OMI MoldUDP64 pcap** (§1.2a) for real NASDAQ MoldUDP64 framing.
4. **Build `chaos` before `recovery`.** Smaller, and gives an immediate first finding: run it
   against *someone else's* feed handler from the Tier 1 list.
5. **Evaluate `jamisonrobey/nasdaq-moldudp64-feed-sim` before writing your own transmitter**:
   testing gap *recovery* is impossible without something to answer a re-request.
6. **`recovery`**: now you have something to test it against. Start from `nassau`'s semantics.
7. **Delta-debug (ddmin) every failure** down to a minimal counterexample before publishing.
8. **`mock-exchange`'s OUCH order-entry part**: heaviest, leave for last.

---

## Note on confidence

**Verified at byte level:** NASDAQ's BinaryFILE framing (24 decoded messages matching ITCH 5.0
struct sizes); IEX HIST is real pcap wire format with VLAN + IEX-TP (20,145 packets, 0 breaks);
the pcap-to-pcapng switch at 20170615/20170620; the OMI MoldUDP64 pcap (399,999 seamless
transitions, 0 breaks); B3 FeedA/FeedB file sizes.

**Verified by reading source:** all 9 Tier 1 targets (file paths and line numbers as recorded in
the table); `jaybeams`'s log-and-continue; `nixiz` being a fork of `helix`.

**Could not verify: noted explicitly, not guessed:** r/cpp rules (Reddit blocks the JSON); the
mechanical-sympathy group's activity; `cppnow.org` (403 bot-block); CFP deadlines for Meeting
C++/ACCU/Core C++/C++ on Sea; the quality of Kaitai's C++ codegen (important: it decides whether a
`.ksy` can generate a fault injector or only a decoder); Glimpse's spin duration (NASDAQ does not
publish it: the "tens of seconds" figure is an estimate).

**Caveat about the sweep:** GitHub caps each query at 1000 results (`MoldUDP64` reports
`total_count=1676`, `SoupBinTCP` 2056: only the top ~100 of each were actually checked);
`search/code` only indexes the default branch; every latency claim in every repo ("39.8ns limit
add", "sub-25ns", "43-cycle deterministic") is **unverified**.

**Still in progress:** the main body of deterministic-simulation-engineering material (C++20
coroutine scheduler design, the list of determinism leaks to plug, loom vs shuttle). Its appendix is
back and lives in Section 10.

---

## SECTION 10: DETERMINISTIC SIMULATION: CRAFT

### 10.1 ⚠️ Don't repeat FoundationDB folklore: a reviewer will catch it

Three things repeated everywhere that are **either wrong or much weaker than their reputation**:

**a) The "one trillion CPU-hours" figure is NOT in the SIGMOD 2021 paper.** It only appears in
`testing.rst`, and that sentence **hasn't changed since commit `3b86576b6`, dated 2018-03-06**:
pre-Apple-era marketing copy, never refreshed. It labels itself an *"equivalent"* estimate,
"weighted by the increased intensity of the failures in our scenarios". And 10¹² CPU-hours ≈
**114 million CPU-years**, which is literally unachievable. If you use it, quote it as
*"an estimate FoundationDB itself writes in its own documentation"*, **never** call it a
measurement, and **never** attribute it to the paper. (The 0.5 million disk-years figure IS real
and is genuinely from the paper, §6.2.)

**b) FoundationDB has NEVER been audited by Jepsen.** The `jepsen.io/analyses` index has no FDB.
The often-cited "FoundationDB passed Jepsen" post is **self-published**, written by a FoundationDB
employee in 2014. Kingsbury's quote exists, but it is a **tweet from 2013-11-25**
(`@aphyr`, id `405017101804396546`, recovered via Wayback): *"haven't tested foundation in part
because their testing appears to be waaaay more rigorous than mine."* **Thirteen words, hedged
twice** ("in part", "appears to be"), from before the Apple acquisition and before the source was
open.

**c) The sentence you should use instead**: Kingsbury, HN 2024 (`news.ycombinator.com/item?id=42120771`),
is the strongest statement of the complementary argument, with one concession:

> *"I've worked on a few projects that used simulation testing and passed with flying colors, and
> still found serious bugs with Jepsen. **Exploring the state space and designing the oracle are
> hard problems.** Jepsen exists to test any third-party database, without their cooperation, even
> without source access. FoundationDB's test suite was designed to test FoundationDB."*

This is the sentence that belongs in your write-up, because it states exactly the limit of the
method you yourself are using.

### 10.2 Shrinker design: a typed choice sequence, not a byte string

**Hypothesis no longer shrinks a byte sequence.** It shrinks a **typed choice sequence**:
`ChoiceNode{type ∈ {integer,float,boolean,bytes,string}, value, constraints, was_forced}`, with
shortlex defined over `(len(nodes), tuple(choice_to_index(v, constraints)))`. A plain
`vector<uint32_t>` works but **loses the most valuable property: typed, constrained choices make
misalignment at replay time DETECTABLE. A raw integer stream would silently misinterpret it.**

Original citation: **MacIver & Donaldson, "Test-Case Reduction via Test-Case Generation: Insights
from the Hypothesis Reducer", ECOOP 2020, DOI 10.4230/LIPIcs.ECOOP.2020.13.** §2.2 is the
seed-vs-tape argument stated formally: *"view the random generator as a parser of a choice
sequence, with the PRNG as the stream interface"*, and §3.2 is the insight that makes it feasible:
*"even though we have no grammar for the language, we do have a parser: the generator itself."*
**Read §2.2 and §3.1-3.3 before writing a shrinker.**

Five design rules, each one earns its place:

1. **Record spans.** Instrument `draw` to push/pop `(label, start, end)`. Without span boundaries,
   deletion is an `O(n³)` candidate search; with them, `O(n)`. RAII guard:
   `auto g = cs.span(EventKind::ApplyDelta);`
2. ⭐ **Never draw a length and then loop.** Draw a "continue?" bool before each element instead.
   Then deleting an element = deleting a **contiguous region**; with a length prefix you must find
   an `O(n²)` pair (shrink the length + delete the region). **This rule alone is the difference
   between a shrinker that works and one that hangs.** Free bonus: `[[1,2],[3,4]]` shrinks itself
   into `[1,2,3,4]`.
3. **`shrink_towards` per draw, with a zigzag index.** `choice_to_index` orders integers as
   `[a, a+1, a-1, a+2, a-2, ...]` around a target specific to each draw, not by raw numeric order.
   Lets you locally declare "the simplest latency is 0", "the simplest instrument is index 0".
   ~30 lines, easy to port elsewhere.
4. **When misaligned, REALIGN, don't abort.** Shuttle *panics* on divergence; Hypothesis instead
   substitutes **the simplest value at index 0** for the type+constraints being requested,
   increments `misaligned_count`, and continues. Running past the recorded prefix is `OVERRUN` =
   "not interesting", never a crash. Report `misaligned_count` per pass as a pass-quality metric.
5. **Two-tier reduction, because your scale is far outside the validated range.** Hypothesis caps
   the choice sequence at `BUFFER_SIZE = 8 * 1024` and the paper itself names this as its main
   threat to validity: *"most test-case reducers run into problems at larger scales that they
   don't see at smaller scales."* A 10M-event run of yours is **far** outside that range.
   **Stage 0:** bisect the deletable suffix (a failure at 10M events usually arises near the end).
   **Stage 1:** causally prune, from the failing event, walk happens-before backward and delete
   every choice not an ancestor of it; **no re-execution needed** if you have recorded dependency
   edges. **Stage 2:** a shortlex pass over what remains, with `max_stall=200`, `MAX_SHRINKS=500`,
   `max_failures=20` per pass, reordering passes by (length deletable, number of shrinks, number of
   calls).

Two extras: a **"shrink open" swarm flag**, drawing a mask over fault types with a baseline of 0
meaning *all on*, so shrinking moves toward *more* faults enabled and the minimal repro is stated
at the most permissive configuration. And a **two-tier corpus** (primary = minimized examples, each
having proven a different bug, sorted shortlex; secondary = a downsample of everything else
interesting, `desired_size = max(2, ceil(0.1 * max_examples))`): just a `.dst-corpus/` directory
holding recorded tapes, committed to the repo.

**On ddmin, for comparison:** guarantees 1-minimality, worst case `|c|² + 3|c|` tests, best case
logarithmic. Measured tradeoff on Csmith: C-Reduce 120 bytes / 3,968 SUT calls, vs Hypothesis 812
bytes / **762** calls. Internal shrinking buys **~5× fewer executions** and always-valid output, in
exchange for larger minima. **For you that is the right trade, because every execution is a
10M-event simulation.**

### 10.3 Oracle: the two biggest time-savers

**a) Partition by instrument, an idea taken from Porcupine's type signature.** Checking global
linearizability is intractable, but the `Model` in `anishathalye/porcupine` has
`Partition func(history []Operation) [][]Operation`: *"a history is linearizable if and only if
each partition is linearizable"* (P-compositionality, Horn & Kroening, arXiv:1504.00204). **Your
order book is P-compositional by symbol almost by definition.** That turns an intractable check
into N independent tractable checks, and it's claimed to be 1,000-10,000× faster than Knossos.
Knossos's additional trick: cache keyed on **(set of operations linearized, model state)**, since
two linearization paths reaching the same *set* into the same state are interchangeable.

**b) TigerBeetle's five checkers, in `src/testing/cluster/`:** `state_checker.zig`,
`storage_checker.zig` (byte-identical storage across replicas at each checkpoint: its header
**lists every excluded region with a reason**, which is the pattern for a determinism oracle that
isn't flaky), `journal_checker.zig`, `grid_checker.zig`, `manifest_checker.zig`, plus
`aof.zig::validate()` (rereads the on-disk log at the end of a run, compares against the final
canonical checksum). **Copy verbatim.**

**The four best value-per-line-of-code oracles for your domain:**

1. ⭐ **Differential snapshot+incremental vs continuous-live.** Run both pipelines on the same event
   stream in the same simulation; the books must agree at every quiescence point. This exactly
   covers the snapshot/recovery path, where real market-data bugs live, and it's the path **least
   run in production**.
2. **Full-book recompute equality.** Naively replay the entire ingest log, compare byte-for-byte
   against the incrementally maintained book. One function, catches every bug involving intrusive
   lists and aggregate maintenance.
3. **Hash chain on the published output stream.** `parent = checksum(previous published message for
   this instrument)`: catches reorder, duplicate, truncate, and divergence in a single check, and
   doubles as a consumer-side canary in production.
4. **Conservation.** `sum(level_sizes) == sum(applied_deltas)`: catches sign errors, double-apply,
   and lost-apply simultaneously.

**Something to take from Jepsen:** `checker/set-full` maintains an add/stable/lost timeline for
each element **specifically to distinguish *lost* from *not yet appeared***: exactly your
dropped-vs-late problem.

**Why the recovery path especially deserves attention**: from sled's error-injection page, citing
Yuan et al., OSDI'14: *"almost all (**92%**) of the catastrophic system failures are the result of
incorrect handling of non-fatal errors explicitly signaled in software"* and *"in **58%** of the
catastrophic failures, the underlying faults could easily have been detected through simple testing
of error handling code."*

### 10.4 Coverage: take the sample metric from your own declared structure

This is the lesson generalized from Coyote: it reports the `(machine, state, event)` tuple as
**covered ÷ defined**, taking `defined` from the handler table the actor itself declares (and ships
a `.coverage.ser` to merge across runs).

Your natural matrix:
`instrument_state ∈ {uninitialized, snapshotting, live, gapped, recovering, stale, halted}`
× `event ∈ {snapshot, delta, trade, heartbeat, gap_detected, retransmit_arrived, session_reset,
subscriber_slow, subscriber_disconnect}` = **63 cells.**

**Print the empty cells.** Every empty cell is either dead code or an untested transition, and you
will be surprised how many are empty.

Note: Stateright independently reinvented the sometimes-assertion as `Property::sometimes` with
`discovery_is_failure() == false`; Resonate independently reinvented it as a *stopping rule* (run
until all 22 operation types have returned a 2xx, then until no new operation signature appears in
20 consecutive batches). **Three unrelated projects converging on the same primitive is a strong
signal that it's correct.**

### 10.5 Canary and mutation table: build on day one

**Canary.** TigerBeetle ships `src/scripts/cfo.zig` with a fuzzer target named **`canary` designed
to ALWAYS FAIL**, with **120 recorded canary failures** in the public ledger. It's a meta-test that
runs forever, auditable by anyone: **if the canary ever passes, the harness is broken.** One
target, unlimited value. **Build it day one.**

**Mutation table**: an artifact that measures **sensitivity** instead of **volume**, and volume
without sensitivity is exactly what let four TigerBeetle fuzzers run through millions of seeds
without ever seeing the zig-zag join bug:

| Mutant | Must violate | Seeds to detect | Wall time |
|---|---|---|---|
| `off_by_one_seq` (accepts `seq == last`) | sequence monotonicity | 1 | 0.3 s |
| `drop_gap_request` (never requests retransmit) | gap-to-recovery liveness | 3 | 1.1 s |
| `stale_snapshot` (delta before snapshot) | dependency order | 12 | 4 s |
| `torn_snapshot` (half-applied on restart) | differential snapshot/live | 40 | 15 s |

Run nightly, and treat **a regression in detection cost as a build failure**: if `stale_snapshot`
used to need 12 seeds and now needs 400, a generator has shrunk. Every real bug you ever fix
becomes a new line. This is exactly what TigerBeetle did **reactively** after #2681 embarrassed
them via Jepsen.

### 10.6 Speed: measure your own, don't inherit "1000×"

| Source | Speedup | Type |
|---|---|---|
| RisingWave | **4-5×** (8 min → 2 min e2e) | **measured**, streaming, CPU-bound |
| FoundationDB | ~10× (*"roughly a factor of 10-to-1 between real time and simulated time"*) | self-reported |
| TigerBeetle | 700-1000× | self-reported |

That gap isn't marketing: it's exactly the CPU-utilization term in FDB's own sentence: *"can run
arbitrarily faster than real time if CPU utilization in the simulation is low, since the simulator
can fast-forward the clock to the next event."* **A bursty feed with long idle gaps and many timers
sits at the favorable end. But measure and publish your own factor.**

TigerBeetle's actual throughput, computed independently from the public ledger
(`devhubdb/fuzzing/data.json`, 479 records): **96,259,250 seeds across 31 commits in one month,
~3 million seeds/day** fleet-wide across 23 targets, with 37 `vopr` failing seeds retained.

### 10.7 Credibility, re-ranked, and volume metrics LAST

1. ⭐ **A post disclosing a bug your DST MISSED, with the mechanism.** TigerBeetle's
   `2025-06-06-fuzzer-blind-spots-meet-jepsen` post is worth more than all the performance numbers
   combined, because it is an artifact that **could have been hidden and wasn't.** Four fuzzers
   missed a zig-zag merge-join path (*"generated objects that happened to sit adjacent in each
   index, so the 'zig-zag' part of the merge join was never exercised"*); VOPR missed two
   corruption bugs because *"it corrupts a whole sector, rather than individual bits."*
2. **Independent third-party validation.** TigerBeetle has a real Jepsen report
   (`jepsen.io/analyses/tigerbeetle-0.16.11`, 2025-06-06) and it **credits their DST**: *"We
   believe this robustness is largely due to TigerBeetle's extensive simulation, integration, and
   property-based testing."* FoundationDB, for all its reputation, has only a 2013 tweet and a
   self-published blog post.
3. A reproducible repro artifact for each failure (hex string or `(seed, commit)`).
4. **A public, always-on seed ledger**: auditability beats assertion; it's exactly what lets the
   ledger in §10.6 be independently verified rather than taken on faith.
5. A mutation-sensitivity table.
6. A postmortem tied to a specific seed.
7. ⭐ **An explicit written statement of the BOUNDARIES of the simulation.** FDB has a §4
   Limitations; Dropbox writes *"Trinity can't actually reboot a machine mid-test, so it can't
   validate that we use fsync in exactly the right places"*; frostdb puts "(mostly)" right in the
   title. **A DST claim without stated boundaries reads as marketing.**
8. Per-run determinism self-check, plus a canary. Dropbox reruns each seed and asserts the same
   final state; Resonate runs every seed twice and diffs the log so that **the nondeterminism
   itself fails the build** (auto-files a real issue: `resonate-sdk-ts#414`).
   **Determinism decays silently.**
9. Coverage gaps are build failures.
10. **Volume metrics: last.**

### 10.8 Closest analogue to your project: Dropbox

Worth reading closely, because it's a **pipeline**, not a consensus system: two harnesses,
**CanopyCheck** on the planner alone (with shrinking and a 200-iteration cutoff for infinite loops)
and **Trinity** on the whole engine (mocks FS/network/timer and **reorders** async requests):
*"tens of millions of randomized test runs"* every night, and **CI auto-files a tracking task for
each failing seed, with the commit hash**. Dropbox is also the **sole exception** to "nobody in the
DST world does shrinking": CanopyCheck has QuickCheck-style shrinking.

### 10.9 Still unverified

The PPoPP 2005 paper on synchronization coverage (DOI 10.1145/1065944.1065972) could not be
obtained from ACM, CiteSeerX, or two mirrors: the citation is certain but the metric description is
a paraphrase, **verify before quoting.** No project documents a systematic mutation-testing program
for a DST harness. No public speed/scale figures for madsim or Antithesis (Antithesis deliberately
declines to publish). `apple/foundationdb/design/testing.md` **does not exist in any ref.** And if
citing Gibbons & Korach for the NP-completeness of linearizability, cite their VL result directly,
don't cite Elle's related-work sentence about it: that sentence actually attributes *sequential
consistency* to them.

---

## SECTION 11: DST ARCHITECTURE IN C++20

### 11.0 ⚠️ Important correction: FoundationDB HAS moved Flow to C++20 coroutines

Verified directly: `apple/foundationdb` on `main` now has `flow/include/flow/Coroutines.h` and
`flow/include/flow/CoroutinesImpl.h` (**1,497 lines**); the number of `.actor.cpp/.h` files dropped
from **500 to 21** between `release-7.3` and `main`; `Net2.actor.cpp` became `flow/Net2.cpp` on
**2026-04-19**. Migration began **2023-11-14**, commit `e07b3e35`, "Added C++ Coroutine support to
Flow". `Future<T>`/`Promise<T>`/`Sim2`/`BUGGIFY` underneath are unchanged.

**This corrects the claim in `RESEARCH-DOSSIER.md` §2.** The correct and stronger way to put it:
*the canonical implementation walked through that door (from the inside) and then welded it shut
behind them.* Flow's coroutines are not separable from `Arena`/`Standalone`/`Reference`/
`g_network`/`allocateFast` and the codebase-wide discipline. **Nobody has extracted it into a
reusable library yet**, and gap searches still return nothing:
`simulation testing coroutine deterministic` → **0**; `virtual clock deterministic scheduler C++`
→ **0**; `deterministic executor coroutine language:C++` → **0**.

**Practical consequence: `flow/include/flow/CoroutinesImpl.h` is the single most valuable file to
read before writing a line of code**: a production DST engine running on native C++20 coroutines.

**Two design decisions in it are literally the answer to your two hardest problems:**

**a) An exhaustively enumerated `await_transform` whitelist, with NO generic fallback.** All 21
declarations in `CoroutinesImpl.h` are concrete overloads (`Future<U>`, `FutureStream<U>`,
`AsyncResult<U>`, `ThreadFutureStream<U>`, `coro::FutureIgnore<U>`, `coro::FutureErrorOr<U,V>`).
**There is no** `template<class A> await_transform(A&&)`.

Because once `await_transform` is declared, *every* `co_await` in the function body goes through
it → `co_await someAsioAwaitable` becomes a **compile error**. This is how they buy correctness **at
compile time**, something the actor compiler never gave them: the actor compiler's error chain only
forbids control-flow shapes it can't lower, it **enforces no determinism at all**.

**b) Frame allocation is in your own hands.** All four promise types declare
`static void* operator new(size_t s) { return allocateFast(int(s)); }`. Plus a placement-new
overload, `FrameSizeRecorder`, to *observe* the frame size the compiler chose, and
`ActorType coroActor; // Embedded in coroutine frame: single allocation`.

### 11.1 Recommendation: write your own `task<T>` + a minimal scheduler, ~800-1500 LOC, no dependency beyond a C++20 compiler

This requirement decides it: **you must own the ready queue, and every scheduling decision must be
a function of the seed and must show up in the trace.** The consequence is that **every library is
disqualified.**

| Repo | ★ | Last push | Verdict |
|---|---|---|---|
| `lewissbaker/cppcoro` | 3,870 | 2024-01-09 | Dormant, 113 open issues. Read to learn. |
| `andreasbuhr/cppcoro` | 449 | 2026-06-12 | Maintained fork |
| `facebook/folly` | 30,480 | 2026-07-29 | Alive; but heavy dependencies |
| `David-Haim/concurrencpp` | 2,757 | 2025-05-01 | Alive |
| `alibaba/async_simple` | 2,192 | 2026-07-08 | **Closest shape.** Read to learn. |
| `jbaldwin/libcoro` | 961 | 2026-05-02 | Alive |
| `Naios/continuable` | 851 | 2023-09-12 | Dormant |
| `NVIDIA/stdexec` | 2,396 | 2026-07-29 | Alive |
| `netcan/asyncpp` | (-) | **404, does not exist** |

Why each one fails: cppcoro's `io_service` uses real OS timers; libcoro's `scheduler` is a thread
pool + `io_notifier`; concurrencpp's thread pool belongs to a `runtime`; asio has `io_context`.
folly's `Executor` seam is **the right shape** but `folly-deps.cmake` requires Boost >=1.69 + fmt +
libevent + OpenSSL + FastFloat. **`async_simple` comes closest** (`virtual bool schedule(Func)`) but
`Func = std::function<void()>` **type-erases away the `coroutine_handle`**, leaving an opaque
closure queue with no coroutine identity to order or trace by. Right shape, wrong type.

### 11.2 ⚠️ P2300 / `std::execution` is in C++26, and it HURTS you

`std::execution::run_loop` is disqualified by three statements in its own spec:

1. `pop-front` / `push-back` are **private and exposition-only** → you **cannot** inspect,
   reorder, seed, or single-step the queue. And **there is no `run_one()`**.
2. The queue is a hardcoded thread-safe FIFO → it explores **exactly one** schedule.
3. Grepping the entire **9,470-line `exec.tex`** for `schedule_after` / `schedule_at` / `now()` /
   `timed_scheduler` → **0 hits**. **C++26 `std::execution` has NO concept of a timed scheduler at
   all**, and a virtual clock is your framework's entire reason for existing.

Senders exist to *abstract away* the scheduler, so a generic algorithm can't depend on it. That is
exactly the property you are **forced to violate**.

On your toolchain (Apple clang 21, `_LIBCPP_VERSION = 210106`): there is **no** `__cpp_lib_senders`,
not even `__cpp_lib_generator` (C++23) at any `-std`. `__cpp_impl_coroutine = 201902`: the core
language has it. **Build on plain C++20.**

### 11.3 Executor shape

```cpp
void enqueue(std::coroutine_handle<> h) { ready_.push_back(h); }   // ONLY enqueue. NEVER resume.

void run() {
  for (;;) {
    while (!ready_.empty()) {
      auto h = pick(ready_);      // THE ONLY PLACE a scheduling decision happens  <- seed enters here
      ready_.pop_front();
      log(step_++, id_of(h));     // THE ONLY PLACE the trace is written
      h.resume();                 // exactly one live resume() at any time
    }
    if (timers_.empty()) break;
    now_ = timers_.begin()->first;  // virtual clock JUMPS. Never sleeps.
    drain_due_timers_into(ready_);
  }
}
```

A single call site for `resume()` gives identical stack depth on every resume, and a total order in
the trace. `pick()` is where the seed enters: FIFO for a baseline, PCT for exploration.

### 11.4 Symmetric transfer: measured, and it's the most dangerous trap

The same program, two forms of `await_suspend`, run on an M2 Max arm64 machine:

```
naive (void await_suspend + resume inline):  100k → OK;  400k / 700k / 1M → exit 139 (SIGSEGV)
symmetric (return coroutine_handle<>):       400k / 700k / 1M → OK
```

**This is exactly your problem, not a corner case:** a simulated network with a virtual clock will
**complete `co_await recv()` synchronously whenever bytes are already in the buffer**: in a
zero-latency simulation that's most of the time. **A correct DST harness is exactly the kind of
workload that runs straight into the trampoline wall.**

Return `coroutine_handle<>` from `await_suspend` of **both** the task awaiter **and** the final
awaiter, and return `std::noop_coroutine()` (**never** a default-constructed handle) when there is
no continuation.

⚠️ **The fact that the naive version survives at 100k is what makes it dangerous: it passes your
unit tests.**

### 11.5 The arena pays off twice

```
arena bump allocator:  2.9 ns/coroutine   (349M/s)
global new/delete:    17.8 ns/coroutine    (56M/s)     -> 6.1x
```

And it makes the heap **checkpointable**: snapshot the arena and diff two runs to localize a
divergence, which is impossible with `malloc`. Add `get_return_object_on_allocation_failure()` so
that arena exhaustion becomes a **deterministic, injectable OOM fault**, instead of a `bad_alloc`
unwinding through the executor.

### 11.6 ⭐ THE LIST OF DETERMINISM LEAKS YOU MUST PLUG

**The pointer-hash leak, the one everyone gets wrong.** Three runs of the same binary:

```
str-iter: NFLX NVDA META TSLA AMZN GOOG MSFT AAPL   (identical across all 3 runs)
ptr-iter: 7 6 1 4 3 2 5 0
ptr-iter: 7 5 3 4 2 1 6 0
ptr-iter: 7 6 5 4 2 3 1 0
```

`unordered_map<string,int>` iteration is **deterministic**; `unordered_set<Node*>` is **not**.
Verified in source: libstdc++'s `bits/functional_hash.h:110-114` is
`hash<_Tp*>{ return reinterpret_cast<size_t>(__p); }`: **the hash IS the pointer**. libc++'s
`__functional/hash.h:344` hashes the pointer's bytes. Neither one **salts** string hashing
(libstdc++ has a fixed seed `0xc70f6907`), so `std::hash<std::string>` is stable, unlike
Python/Rust.

**And here is the crucial part: a deterministic arena fixes ordered containers but does NOT fix
hashed containers** (4 runs):

```
run 1: arena base=0x104904000  hash-by-POINTER: 7 6 5 1 4 3 2 0   hash-by-ID: 7 6 5 4 3 2 1 0
run 2: arena base=0x100de4000  hash-by-POINTER: 7 6 5 4 0 2 1 3   hash-by-ID: 7 6 5 4 3 2 1 0
run 3: arena base=0x102474000  hash-by-POINTER: 7 6 5 0 3 2 4 1   hash-by-ID: 7 6 5 4 3 2 1 0
run 4: arena base=0x102d84000  hash-by-POINTER: 7 5 4 3 2 6 1 0   hash-by-ID: 7 6 5 4 3 2 1 0
```

`std::set<Node*>` over arena memory is *stable* (relative order survives a base shift), but
`unordered_set` computes `bucket = ptr % 11` and ASLR shifting the base changes the modulus.

> ⭐ **The rule is NOT "use an arena". The rule is: never let a pointer value reach a hash function
> or a comparator. Give every object a monotonic ID from a seeded counter, and order by that ID.**

FDB reaches the same conclusion; `contrib/debug_determinism/README.md` lists it as failure mode #2:
*"Depends on the relative order of allocated memory. E.g. using a heap pointer as a key in
`std::map`."*

Worse: `std::set<Node*>` over **heap** pointers is **inconsistent**: run 3 happens to come out in
the correct order while runs 1-2 don't. **Inconsistent nondeterminism is the hardest kind to
debug.**

| # | Leak | How to plug it |
|---|---|---|
| 1 | `system_clock`, `steady_clock`, `high_resolution_clock`, `clock_gettime`, `gettimeofday`, `time()` | Inject a `Clock` concept / template param. Ban the header in the DST build. |
| 2 | **`rdtsc`**: an *instruction*, not blockable by a linker or `LD_PRELOAD` | Shim only at compile time; **grep the disassembly to prove it isn't there** |
| 3 | `random_device`, `rand()`, `getrandom`, `getentropy`, `arc4random`, a default `mt19937` seed | All randomness through exactly one `ChoiceSource` |
| 4 | **`std::uniform_int_distribution` and its relatives**, `[rand.dist.general]` **does not mandate an algorithm**, so a seed doesn't replay identically between libstdc++ and libc++ | Write your own `draw(bound)` using only integers. TigerBeetle goes further: `stdx.PRNG` has **no floating point at all**, uses `Ratio{num,den}` and `chance()` on integers |
| 5 | Hashing derived from a pointer (`unordered_*<T*>`) | Never hash a pointer. Monotonic ID. |
| 6 | **Order** derived from a pointer (`std::set<T*>`, `std::map<T*,V>`, sorting on pointers, pointer as tie-break) | Total order on ID; the arena is a second line of defense |
| 7 | ASLR / absolute addresses leaking into a log or comparison | Log the arena **offset**, never the absolute address. Even Shadow's own determinism test has to `sed 's/0x[0-9a-f]*/HEX/g'` before diffing: that is itself a tell. |
| 8 | `std::thread`, `std::async`, thread ID, `hardware_concurrency`, `sleep_for` | A single OS thread. Ban at build time **and** add a runtime tripwire |
| 9 | Syscall ordering: `epoll`/`kqueue` return order, short reads, `mmap`, DNS, TCP coalescing | Abstract at the **message seam**, not the byte seam |
| 10 | `readdir` ordering: confirmed that `directory_iterator` returns `alpha bravo charlie zeta mike yankee`, i.e. filesystem order, not sorted | Sort explicitly, always |
| 11 | **FP contraction**: verified at instruction level: `-ffp-contract=off` → `fmul d0,d0,d1`; `on` (**Clang's default**) and `fast` (**GCC's C++ default**) → `fmadd d0,d0,d1,d2` | `-ffp-contract=off`, not `-ffast-math`/`-Ofast`, recorded in the build fingerprint. **Critical for a price/VWAP pipeline.** |
| 12 | libm version drift (`sin`/`exp` not bit-identical across versions), `long double` size, denormal/FTZ, `accumulate` ordering | Avoid transcendentals in the deterministic core; **fixed-point for prices** |
| 13 | **Uninitialized memory.** FDB: *"99/100 times, the source of nondeterminism is using uninitialized memory"* | MSan/valgrind **inside** the DST loop |
| 14 | Struct padding in a struct that gets hashed/serialized/`memcmp`'d | Serialize explicitly; never `memcmp` a struct |
| 15 | Undefined permutation from `std::sort` with equal elements | `stable_sort`, or a total order with no ties allowed |
| 16 | `type_index` ordering: stable within one static binary, unsafe across shared libraries | Don't order by it; static-link the DST binary |
| 17 | Env vars, static-init order across TUs, `__FILE__`/`__COUNTER__` in compared output | Explicit init sequence; canonicalize logs |
| 18 | Any app logic reading wall time: timeouts, rate limiters, TTL caches | All of it through the virtual clock |
| 19 | **Compiler/stdlib version: reproducibility only holds per binary.** Verified: the same 16-frame coroutine chain allocates a total of **704 bytes at `-O0`, 608 bytes at `-O2`** | Record a build fingerprint (compiler, version, flags, stdlib) alongside every seed. **A seed without a fingerprint is not a repro.** |
| 20 | HALO elision silently skipping your `operator new` | Verified both ways. Don't rely on it, and **never share a single PRNG stream between the allocator and the fault injector**: an elided allocation shifts the whole tape. |

**Safe containers:** `vector`, `deque`, flat maps, intrusive lists, a B-tree keyed on a total order
over IDs. **Traps:** any `unordered_*` keyed on a pointer; any ordered container keyed on a
pointer; anything whose iteration order you haven't explicitly established.

**Two detectors to ship on day one, both from FDB:**

- **Unseed fingerprint:** draw an integer from the PRNG at process exit, log it, rerun the seed,
  compare, a single-integer hash of "how much randomness was consumed, and in what order", sampled
  via `unseed_check_ratio` with an explicit opt-out for tests that are inherently nondeterministic.
- **`contrib/debug_determinism/`:** build with `-fsanitize-coverage=trace-pc-guard`, write every
  edge id to a file, rerun, **halt at the first differing edge** to attach a debugger at exactly
  that spot (~50 lines).

### 11.7 loom vs shuttle: build RANDOMIZED/PCT as the main engine

**Don't build DPOR. Don't build a C11 memory-model simulator.** Add exhaustigen-style bounded
exhaustive testing for small units, since it is nearly free.

**Five reasons:**

1. **loom's limits are data-layout constants, not knobs.** `MAX_THREADS = 5` **counts main**
   (`VersionVec` is `[u16; 5]`); `MAX_ATOMIC_HISTORY = 7`, so a bug that needs the 8th store is
   **invisible**; the default is 1,000 branches with a panic on overflow. Your pipeline is feed
   handler → decoder → book builder → fan-out: **many actors, long runs**.
2. **"Exhaustive" ≠ sound, and loom says so itself.** SeqCst is modeled as AcqRel → **false
   positives**; load buffering isn't explored → **false negatives**. You pay the full price of DPOR
   plus the memory model **and still get no proof**.
3. **The bugs you care about are SHALLOW.** In the PCT paper's evaluation, **every** bug has depth
   d ∈ {1,2}, including two previously-unknown browser bugs (Mozilla, IE) found at d=1 in programs
   with `n=25, k=1.4M` and `n=12, k=38.4M`. Compared to CHESS: PCT finds the same two bugs at run 6
   and run 35, where CHESS needs ~200 and ~1000. **Stress testing gets 0.**
4. **Microsoft's own C++ port made this same choice.** `cpp-systematic-testing` ships
   `StrategyType { Random, Prioritization, Replay }`: **no DFS, no memory model**.
5. **Stackless coroutines argue the same direction.** loom needs a branch point at *every* atomic,
   deep inside the call stack: that's exactly why both loom and shuttle use **stackful**
   coroutines. C++20 coroutines are **stackless**: you can only suspend at a `co_await` in your own
   frame. **PCT only needs a decision at the suspension point, which is exactly where `co_await`
   already is.** Budget for the consequence: `co_await` propagates, so every blocking primitive
   must become awaitable.

**PCT (Burckhardt, Kothari, Musuvathi, Nagarakatte, ASPLOS 2010).** Assign each task a **random
priority at creation**; always run the highest-priority runnable task; insert `d - 1` **randomly
chosen priority-change points**, and at the i-th point lower the running task's priority to `i`.
Guarantee, verbatim: *"Given a program that creates at most n threads and executes at most k
instructions, PCT finds a bug of depth d with probability at least **1/nk^(d-1)**."* For d=1, d=2
that's `1/n` and `1/nk`. The measured rate in practice is up to **4 orders of magnitude** higher
than the bound (Dryad: 0.164 measured vs 2×10⁻⁵ guaranteed).
PDF: `microsoft.com/en-us/research/wp-content/uploads/2016/02/asplos277-pct.pdf`

**Implementation details worth copying from shuttle** (`shuttle-schedulers/src/pct.rs`): **learn
`k` dynamically** (iteration 0 measures the step count, adjusts upward when a longer run is seen:
this kills the paper's biggest practical problem); **only count a step when
`runnable.size() > 1`** (the phase optimization from §4.1; their Table 2 shows `k` dropping from
1.4M→0.13M and 38.4M→3M, a direct 10× improvement at d=2); insert a **"Final Wait"** point from the
paper before the main task exits; treat `is_yielding` as a forced priority change. Add Coyote's
**fair suffix** (an unfair PCT prefix then a uniform-random tail, `MaxFairSteps = 10 ×
MaxUnfairSteps`) so spin loops and liveness assertions don't create false livelocks. Then run a
**portfolio** of multiple strategies × multiple seeds: Coyote's honest answer to "which strategy?"
is "several of them."

**A primitive that dissolves the question.** Route every nondeterministic decision through a single
narrow interface: shuttle proves three methods suffice (`new_execution()`, `next_task(runnable,
current, is_yielding)`, `next_u64()`): then swap the implementation:

| Mode | Implementation | Gives you |
|---|---|---|
| Generate | seeded xoshiro256++, **records** each draw | DST randomized at scale |
| Replay | replays the recorded sequence | reproduction, CI regression |
| Shrink | replays a **mutated** sequence | minimal counterexample |
| Exhaustive | an odometer over `(value, bound)` pairs | bounded exhaustive (loom mode), **~90 LOC** |

TigerBeetle ships the exhaustive one: `src/testing/exhaustigen.zig` presents *the same API as
their PRNG* but enumerates every choice sequence: keeps `(value, bound)` pairs; to advance,
increments the rightmost value still under its bound and zeroes the rest (**a mixed-radix odometer
over a dynamically discovered space**). Its test asserts exactly n! permutations of "abcd". Depth
capped at 32. The reasoning, from matklad's "generate all the things" post: the sequences come out
**already ordered by complexity**, so *"the first failing example is in fact guaranteed to be the
smallest counterexample"*: **free shrinking at small scale**.

### 11.8 How FDB and TigerBeetle virtualize, and where the seed flows

**FoundationDB.** `g_network` is a **swappable global**: `Net2` in production, `Sim2` in
simulation. All time comes from `g_network->now()`; all I/O goes through that same interface.
**Nothing in application code names a real clock.** That single layer of indirection is the entire
architecture, and it's why the move to coroutines didn't disturb it: `Net2.cpp` changed, `Sim2`
didn't need to.

Randomness is **named apart so misuse becomes visible**: `deterministicRandom()` vs
`nondeterministicRandom()`: two functions, and the second one is a code smell you can grep for.

Storage: `fdbrpc/AsyncFileNonDurable.actor.h` models unsynced writes being lost, writes not yet
`fsync`'d possibly being lost, partially applied, or reordered on a simulated crash.

**Network fault: "swizzle-clogging"**, their signature pattern, verbatim from `testing.rst`:
*"pick a random set of nodes... 'clog' (halt) each of their network connections, one at a time,
for a few seconds... then unclog them in random order."* **Should be copied directly**: it creates
asymmetric, time-varying partitions that uniform drop probabilities never reach.

⭐ **BUGGIFY: two-tiered, and this is the key design.** Verified in `flow/include/flow/Buggify.h`:
`P_BUGGIFIED_SECTION_ACTIVATED = 0.25` is decided **once per (file,line) per run** and memoized in
`*_SBVars`; `P_BUGGIFIED_SECTION_FIRES = 0.25` is evaluated **every time** that site runs.

That is **swarm testing at line-of-code granularity**: each run activates a random ~25% subset of
sites, keeping each run survivable while the whole set still covers the cross-product. **532 sites
across 66 files on `main`** (778 on `release-7.3`, pre-migration). It's now a *function* using
`std::source_location`, not a macro. `P_EXPENSIVE_VALIDATION = 0.05` rides along with that switch
so O(n²) invariant checks turn on in 5% of runs. Activation decisions are traced, so a failing
run's exact activation set is recoverable. Alex Miller (`transactional.blog/simulation/buggify`)
states the intent plainly: ***"do bad things, but not too much."***

**Swarm the configuration too, not just the faults.** From `testing.rst`: *"Randomizing tuning
parameters as well ensures that specific performance-tuning values don't accidentally become
necessary for correctness."*

Coverage: `CODE_PROBE` (**685 sites on `main`**) with static registration per call site, so a probe
**never hit still shows up** in the output. Aggregated fleet-wide by `contrib/TestHarness2` **into
FoundationDB itself** via a conflict-free atomic counter, `tr.add()`, keyed on
`(file, line, comment, rare)`: **keyed by comment, so refactoring doesn't reset history**.

**TigerBeetle.** Time is tick-based, and **the clock is a first-class component that can be
faulted**: per-replica skew and drift are injected so their own clock-sync code (`src/clock.zig`)
gets tested rather than assumed. Sub-tick I/O is drained by a **quiescence loop**:
`while (advanced) { network.step(); storage.step(); }`.

⭐ **The seed is a git hash.** `if (bytes.len == 40)` parses it into a u160 then truncates, so
**every commit gets a free reproducible run**, and every failure is a `(seed, commit)` pair.
`src/scripts/cfo.zig` keeps `commit_count_max = 32`, `seed_count_max = 4`, `budget = 60 minutes`,
`timeout = 30 minutes`, and **prioritizes faster-failing seeds** (comment: `as coarse seed
minimization`).

**A single PRNG, integer only.** `stdx.PRNG` exposes `Ratio{num, den}` and `chance()`: **no
floating point anywhere in the random API**, eliminating leaks #4 and #11 by design.

**Swarm at the enum level.** `fuzz.random_enum_weights` **completely disables a random subset of an
enum's variants** for a run.

⭐ **Liveness vs safety: their most transferable idea.** Distinct exit codes (`crash=127`,
`liveness=128`, `correctness=129`). A stall detector that **resets on progress**:

```zig
if (simulator.requests_replied > requests_replied_old) tick = 0;  // tick COUNTED FROM LAST REPLY
```

Then a two-phase approach: run with faults; then pick a **core** (a strongly-connected component
containing a quorum), **heal it completely** (1 ms latency, zero fault probability), and require
convergence within `ticks_max_convergence = 10_000_000`. `pending()` returns `?[]const u8`: **a
REASON string**, not a bool. Crucially: `cluster_recoverable()` must *prove* that a stall is
explained by an injected fault **before** reporting a bug: ~170 lines reconstructing the cluster's
view and **panicking with `"block found in core"`** if a replica really is holding a block another
replica is waiting on.

**A speed-up move.** The `test_min` config shrinks the state space (`journal_slot_count = 64`,
`block_size = 4096`, `lsm_compaction_ops = 4`) so **a full lifecycle**: checkpoint, WAL wrap,
compaction, state sync, is reachable in seconds.

### 11.9 Performance model, derived empirically

Virtual time **jumps to the next event**, so wall time is O(#events), **independent of simulated
duration**. Measured 15-29M coroutine resumes/second single-threaded (10M events in 0.39-0.65 s),
then just varying the mean virtual gap between events:

```
GAP=10 ticks      → 0.4x real time
GAP=1,000 ticks   →   46x
GAP=100,000 ticks → 4418x
```

**`speedup ≈ mean_virtual_gap_per_event / wall_time_per_event`**, with wall_time_per_event ≈
**34 ns**. **To claim 1000× you need ≥ ~34 µs of virtual time per global event.** This explains the
published gaps: RisingWave 4-5× (measured, CPU-bound), FDB ~10× (self-reported, busy DB, dense
events), TigerBeetle 700-1000× (self-reported, many long idle stretches).

### 11.10 Shadow: a verdict, NOT your substrate

`shadow/shadow` is alive (1,712★, pushed 2026-07-28). Counting the dispatch table in
`src/main/host/syscall/handler/mod.rs`: **exactly 150 `SyscallNum::NR_* =>` branches**, including
`clock_gettime`, `getrandom`, `nanosleep`, `epoll_*`, `futex`.

**But read `docs/testing_determinism.md`, which states it honestly:** *"If you run Shadow twice
with the same seed... it **should** produce a deterministic result (**if not, that's a bug**)."*
Determinism here is an **aspiration with a bug tracker**, not a guarantee. And `docs/limitations.md`
notes that `--native-preemption-enabled` causes *"loss of simulation determinism."*

**Six reasons to disqualify it:** (1) Linux-only, and `LD_PRELOAD` means **a statically linked
binary won't run at all**, while you develop on macOS/arm64; (2) **no in-process scheduling
control**: it *"runs each thread until it blocks on a syscall"* and *"models the CPU as infinitely
fast"*, so **every concurrency bug in your recovery layer is invisible**; (3) no in-process
invariant hook, which is where most of TigerBeetle's oracle lives; (4) no event-level replay
artifact, no shrinking; (5) **can't inject storage corruption**: no torn writes, no bit flips
(exactly the fault class that embarrassed TigerBeetle's VOPR); (6) documented gaps that
**specifically bite a market-data pipeline**: no IPv6, no `sendfile`, **no `SO_REUSEADDR`**, no
`TCP_FASTOPEN`, `vfork` implemented as a synonym for `fork`, and **a busy loop deadlocks the whole
simulation**.

**Where it does deserve a place:** as a **second, supplementary harness**, to test a third party's
feed handler **as intact processes** on a real TCP/UDP stack, i.e. the half of your strategy shaped
like Jepsen. Frame it that way, **don't** say "arguably repurposable rather than rebuilt" the way
the old dossier did.

**A verified nearby alternative:** `facebookexperimental/hermit` (1,392★) is closer to what you
want: it controls *"thread scheduling, time, random data, CPUID results, and selected file
metadata"* via ptrace/Reverie, but the README says **"Hermit is in maintenance mode"**, x86-64
Linux only, and installation points to a personal fork. `rr` (10,602★) supports Apple Silicon
M-series *microarchitecture* but **only under Linux with PMU virtualization**, which most cloud
VMs don't expose, and rr replays **a single observed execution** while DST **explores many**: a
different class of tool.

### 11.11 Demos built and measured

All in the scratchpad, built with Apple clang 21 / libc++ 210106 on an M2 Max arm64:

| File | Demonstrates | Measurement |
|---|---|---|
| `det3.cpp` | ⭐ **The decisive one.** With a deterministic arena, hash-by-pointer is **still** nondeterministic across 4 runs; hash-by-ID is fully stable | see §11.6 |
| `trampoline.cpp` | The necessity of symmetric transfer | naive **exit 139** at 400k/700k/1M, **OK at 100k**; symmetric OK at 1M |
| `arena_coro.cpp` | `promise_type::operator new` **does** catch frame allocation; frame layout is **not** ABI-stable across `-O` levels | 16 frames at both levels, **704 B at `-O0` vs 608 B at `-O2`** |
| `allocbench.cpp` | Arena vs global `new`; and **HALO elision is real but unpredictable** | **2.9 ns vs 17.8 ns (6.1×)**. The first version got completely elided at `-O2` (`operator new` never ran at all) |
| `bench.cpp` | Virtual-clock executor throughput and the density model | **15-29M resumes/second**; 0.4× / 46× / 4418× |
| `shrink.cpp` | End-to-end PoC: a `ChoiceSource` with Generate/Replay modes, a shortlex shrinker over a toy feed handler with a planted bug | tape **14 → 4** choices in **43 reruns**; minimal counterexample `[2,1,1,1]` = snapshot-start, gap, gap, gap, **the shrunk tape IS the bug report** |
| `fp2.cpp` | FP contraction at the instruction level | `off` → `fmul`; `on` (default) and `fast` → `fmadd` |
| `det.cpp`, `det2.cpp` | `hash<string>` stable across processes; `set<Node*>` over a heap pointer nondeterministic **and inconsistent** | run 3 happened to come out in the right order: a dangerous case |

Caveat on `shrink.cpp`: half of the seed-perturbation experiment is degenerate (the toy bug is too
easy to hit, so 20/20 adjacent seeds also fail). The "a seed has no gradient" argument is a
**structural argument**: changing the seed re-randomizes every downstream draw, it is not something
the toy actually demonstrates. The tape-shrinking half is solid, and that is the main point.

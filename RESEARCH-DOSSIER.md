# Quant Dev Portfolio: Landscape Research & Problem Selection

Research date: 2026-07-29. Four parallel research streams (OSS supply survey, practitioner pain
mining, demand-side/hiring analysis, tooling-gap deep dive) plus independent verification of
load-bearing claims. Constraints as given: **cloud VMs only, open-ended timeline.**

---

## 1. The saturation data — this is the whole argument

All counts from the authenticated GitHub API, 2026-07-29/30.

| Query | Repos |
|---|---|
| `"order book" language:C++ created:>2026-01-01` | **1,071** |
| …of those, with **≥5 stars** | **7** (0.65%) |
| `"matching engine" language:C++ created:>2026-01-01` | **748** |
| `"limit order book" language:C++ stars:>=50` (all time) | **3** |
| `itch nasdaq` in C++ | **96** (~70% pushed in the last 6 months, 0 stars) |
| **The other side of the ledger** | |
| `"AF_XDP" multicast` | **0** |
| `"gap fill" multicast market data` | **0** |
| `glimpse soupbintcp` | **0** |
| `"OUCH protocol nasdaq"` | **0** |
| `shared memory pub sub bus low latency language:C++` | **0** |
| `mock exchange gateway trading` | **0** |
| `market data feed simulator multicast replay` | **0** |
| `"queue position" backtest fill probability` | **0** |
| `packet timestamp correlation trading` | **0** |
| `deterministic scheduler test language:C++` | **0** |
| `"CME MDP 3.0"` | **8** |
| `feed arbitration multicast market data` | **1** (0★) |
| `deterministic replay trading system` | **7** (best real one: 1★) |

**1,071 C++ order books in seven months. Zero multicast gap-fill libraries, ever.**

That asymmetry is the finding. Everything that *decodes* a feed or *matches* an order is
saturated to statistical invisibility. Everything that models **the counterparty side** — the
queue, the exchange's behaviour toward your order, the recovery path, the wire, the clock — is
empty.

### The correctness study that proves the ceiling

[flash1-dev/matching-engine-benchmark](https://github.com/flash1-dev/matching-engine-benchmark)
(27★, MIT, pushed 2026-07-20) ran a deterministic ~2M-message workload against **247 matching
engines in 20+ languages**:

- **47 conform as shipped**
- 110 conform only after a documented fix
- **87 diverge, crash, or are infeasible**
- **181 GitHub issues filed, 250+ findings, 18 fixed upstream, none declined**

Named defects in the *popular* repos: `Kautenja` (311★) — use-after-free on cancel.
`slmolenaar` (266★) — swap-and-pop breaks FIFO time priority. `lanpishu` (387★) — broken RB-tree
delete-fixup. The single most common bug class, found independently in ten engines: **fills
priced at the aggressor's limit instead of the resting maker's** — which is not an optimization
detail, it is the definition of a limit order book.

Caveat: Flash One is a patent-licensing business and reports its own engine fastest. But the
correctness oracle is byte-identical consensus first established from three independent
third-party engines (liquibook, QuantCup, exchange-core), and every per-engine finding is
checkable in source. The methodology stands independent of their commercial interest.

**Read this as calibration, not discouragement.** It is proof that (a) the saturated space is
saturated with *broken* code, and (b) a rigorous differential-testing artifact in this domain
gets 181 issues filed and 18 upstream fixes — i.e. it lands.

---

## 2. Distinctiveness, diagnosed properly

An order book fails not because it is easy, but because **it carries no falsifiable claim.** It
is an artifact whose quality a reviewer cannot assess in five minutes.

- *"I wouldn't 'showcase' it in a resume"* — insider, TeamBlind 2021
- *"Literally a Python dictionary with lists for each price. Can be done in 10 mins."*
- *"is there like a popular YouTube video going around? It seems like there's a big spike in the
  number of students and new grads wanting to show off their TPP matching engine benchmarks"*
  — r/highfreqtrading, 2026-01-16
- *"Full stack HFT projects are dime a dozen."*

**The unit of distinctiveness is a falsifiable claim, not a codebase.** Compare *"I built a
low-latency order book, 2.5 billion TPS"* against *"here is a deterministic fault schedule that
makes six published ITCH feed handlers silently produce a wrong book, reproducible from seed
4711 in one command."* The second is verifiable in a minute, novel, and demonstrates the exact
competencies the job requires.

### Verified independently by me (own reads, not relayed)

- `rigtorp/MPMCQueue` README TODO: `- [X] Add allocator supports so that the queue could be
  used with huge pages and shared memory` — marked **done**.
- `include/rigtorp/MPMCQueue.h:278` — `Slot<T> *slots_;`. A raw pointer data member, so the
  control block is **not position-independent**: map it at a different address in a second
  process and it breaks.
- `grep -niE "shared memory|shm|interprocess|process"` over the header → **zero matches**.
- Maintenance: MPMCQueue 1,555★ last pushed 2024-03-08; SPSCQueue 1,269★ 2024-01-04. Both
  unmaintained. `max0x7ba/atomic_queue` (1,880★, active) documents position-independence
  correctly, but its runtime-sized `AtomicQueueB*` variants use `std::allocator` and silently
  forfeit it — undocumented.

A real, checkable finding sitting in plain sight in a repo with 1,555 stars. Cost: one `curl`
and one `grep`.

### The C++ concurrency-testing void, verified by me via the GitHub API

| Repo | Stars | Last push | Status |
|---|---|---|---|
| `microsoft/cpp-systematic-testing` | 44 | **2022-09-29** | **archived: true**, 2 commits ever |
| `microsoft/coyote` | 1,589 | 2024-12-11 | dormant 19 months |
| `mpdn/unthread` | 48 | 2023-05-07 | dead; single-core, no preemption, no `std::thread` |
| `tokio-rs/loom` | 2,767 | 2026-02-20 | alive |
| `awslabs/shuttle` | 1,036 | **2026-07-28** | alive |
| `madsim-rs/madsim` | 1,139 | 2026-02-16 | alive |
| `tokio-rs/turmoil` | 1,230 | 2026-07-21 | alive |

Rust has four healthy options; the JVM has Lincheck and Fray. **C++ has one dead 48-star pthread
shim** — the sole entry in the C/C++ section of `awesome-deterministic-simulation-testing`.
Microsoft's own attempt is archived.

**⚠️ CORRECTION (2026-07-30), and it makes the framing stronger.** An earlier version of this
section said "C++20 coroutines removed the barrier that made Flow a dialect — nobody has walked
through the door." **Someone did.** Verified directly: `apple/foundationdb` on `main` now has
`flow/include/flow/Coroutines.h` and `flow/include/flow/CoroutinesImpl.h` (1,497 lines);
`.actor.cpp/.h` files went **500 → 21** between `release-7.3` and `main`; `Net2.actor.cpp` became
`flow/Net2.cpp` on 2026-04-19. The migration began 2023-11-14, commit `e07b3e35`, "Added C++
Coroutine support to Flow." `Future<T>`/`Promise<T>`/`Sim2`/`BUGGIFY` are unchanged underneath.

The correct and more defensible framing: **the canonical implementation walked through the door
internally and welded it shut behind them.** Flow's coroutines are inseparable from
`Arena`/`Standalone`/`Reference`/`g_network`/`allocateFast` and whole-codebase discipline.
**Nobody has extracted it as a reusable library**, and the void searches still return nothing:
`deterministic simulation testing language:C++` → 4 repos, all 0★ and unrelated;
`simulation testing coroutine deterministic` → **0**; `virtual clock deterministic scheduler C++`
→ **0**; `deterministic executor coroutine language:C++` → **0**.

Practical consequence: `flow/include/flow/CoroutinesImpl.h` is now **the single most valuable file
to read before writing any code** — a production DST engine on native C++20 coroutines. See
`BUILD-GUIDE.md` §11 for the two design choices in it that answer the hardest problems.

Supporting: TSan **cannot see weak memory**. Clang's own `force_seq_cst_atomics` flag is the
admission — TSan observes one real execution on x86-TSO, which hides the very reorderings your
`relaxed`/`acquire` reasoning depends on. It will pass a queue that breaks on Graviton, and its
Limitations section never says so. Most practitioners believe green TSan means correct.

---

## 3. Do not build these

| Space | Verdict |
|---|---|
| Limit order books / matching engines | 1,071 in seven months. Actively mocked. |
| ITCH **decoders** | 96 in C++, ~70% from the last 6 months at 0★. Every one says "zero-copy, low-latency, C++20." |
| Backtesters | ~4,294 repos; L2-replay fill rates are provably optimistic without MBO queue position. |
| "Fast FIX parser" | Credibility-poor, not prior art. `NexusFix` (96★) claims sub-100ns with no CPU model, no p99.9, 8 message types, no CI. |
| Crypto bots | HRT: *"we probably won't believe you."* Optiver: *"awww that's so cool look at you go."* |
| Throughput headlines | Jump: *"the benchmarks don't capture what happens in a production setting."* |
| Anything LLM-smelling | An HFT CTO publicly called out a public repo as LLM-generated (HN 46387677). HRT screens for it. **Actively counterproductive.** |

Also: `PacktPublishing/Building-Low-Latency-Applications-with-CPP` (688★) is Sourav Ghosh's book
code — a complete teaching trading ecosystem that candidates increasingly clone and present as
their own. **Assume interviewers recognize it.** And `0burak/imperial_hft` — the
second-highest-starred "HFT C++" repo on GitHub at 1,230★ — is **nine commits** of technique
tips with no license.

---

## 4. My reservations about the obvious answer

All four streams initially converged on "build a percentile-honest latency measurement rig."
That was my recommendation before the hardware answer. **On cloud VMs only, I no longer
recommend it as the headline**, for four reasons:

1. **You cannot answer the standard objection.** The most common public correction issued to
   anyone publishing a latency number is *"you used software timestamps, not wire-to-wire
   hardware timestamps."* Cloud vNICs have no hardware timestamping. You would be publishing
   precisely the class of number that gets mocked, with no defence available.
2. **The measurement substrate is absent in cloud on every axis**: no PMCs (so no `rdpmc`, no
   PEBS), no Intel PT (so no flight-recorder snapshot), trapping `cpuid`, a slow virtual
   clocksource, no flow steering, and on Graviton no C-state or frequency control at all. A
   practitioner's blunt verdict: *"sub-micro timings are indeed all over the place on a cloud VM
   which is why you don't use them for anything where that matters."*
3. **A measurement tool needs a workload**, and the workload is the saturated thing. The rig
   alone is not a project.
4. **"I built a better benchmark" is tool-shaped.** Interviewers reward findings, and the
   interesting findings in that space require trustworthy hardware.

**The constraint is clarifying rather than limiting.** It rules out the entire
"measure-nanoseconds-on-tuned-bare-metal" class — which is exactly the class that is both
saturated with unverifiable claims and impossible to do credibly without hardware you don't
have. What survives is *also* what the survey ranks as the emptiest and highest-status space:

> *"The strongest signal is test and recovery infrastructure, not hot-path speed. These are
> things a firm has definitely built internally, definitely not shared, and will immediately
> recognize as the work of someone who has shipped rather than someone who has read."*

Correctness, recovery, determinism and protocol semantics are **hardware-independent by
construction** — determinism is *simulated*, not measured. A cloud VM is a perfectly legitimate
place to do all of it.

---

## 5. Recommended thesis: build the counterparty

> **A deterministic exchange-simulation and market-data recovery stack in C++.** Build the thing
> that behaves like an exchange, and the thing that survives an exchange behaving badly — with a
> seeded deterministic simulator that makes both reproducible.

Three composable components. Each is an independently verified void. None needs special
hardware. Together they close a loop.

### Component 1 — A mock exchange that speaks real wire protocols

The gap, verbatim from the survey: *"There is no maintained OSS mock exchange that speaks real
ITCH over multicast + OUCH/SoupBinTCP over TCP, runs a price-time-priority engine, and injects
realistic per-hop latency."*

The direction of travel is the whole point: **ITCH *decoders* are saturated to absurdity; ITCH
and OUCH *encoders that behave like an exchange* number roughly zero.** Everyone builds ingress.
Nobody builds egress, which is where the hard correctness problems live — session sequencing,
replay-on-reconnect, sequenced vs unsequenced packets, in-flight order state across reconnect,
cancel-on-disconnect.

Supporting gap #4: `"OUCH protocol nasdaq"` → **0 repos**. The only dedicated C++ OUCH library
is 8★ and dead 11 years. The best C++ SoupBinTCP implementation in existence
(`bobbleclank/soupbintcp`) has **three stars**.

Prior art to mine, not copy: `paritytrading/parity` (502★) was a complete OSS equities exchange —
**archived 2022-02-09**. `mkipnis/DistributedATS` (115★, active) is the most architecturally
complete C++ thing in OSS (QuickFIX + LiquiBook + FastDDS, real gateways, `LatencyTest`) but is
FIX-only with no binary multicast feed, single maintainer. `libtrading` (738★, dead 5.5 years,
never replaced) has the most protocol-complete headers anyone has open-sourced — ten exchange
protocols, both directions, session layers included.

### Component 2 — The recovery layer (ranked #1 by how much a firm would care)

Gap detection + retransmit/rewind server + Glimpse-style snapshot/refresh + **A/B line
arbitration** + NAK suppression, as a reusable C++ library.

- `"gap fill" multicast market data` → **0 repos**
- `glimpse soupbintcp` → **0 repos**
- `feed arbitration multicast market data` → **1 repo, 0 stars**

Firms pay Informatica Ultra Messaging six figures a year largely for exactly this. Aeron
(8,765★) gives you reliable transport and **zero market-data recovery semantics**. Note also
that Aeron Cluster has no C or C++ implementation at all — `aeron-cluster/src/main/` contains
only `java` and `resources`. This is a real, expensive, unsolved layer.

### Component 3 — The deterministic fault injector and simulator

This is the differentiator, and it is where the C++20-coroutine DST framework from §2 lives.

Protocol-aware, seeded, deterministic fault injection: drops, reorders, duplicates, A/B
divergence, sequence resets, mid-session disconnects, snapshot-vs-incremental races. Rewrites
MoldUDP64 / SBE sequence numbers and timestamps so a receiver's gap logic is genuinely
*exercised* rather than merely confused. Speed-scales while keeping protocol invariants valid.
Replays from a seed.

Gap #3 in the survey, described as the **best gap-to-effort ratio on the list**. `tcpreplay`
(1,336★) is protocol-blind — it cannot rewrite a MoldUDP64 sequence number or arbitrate A/B
lines. The de facto market-data replay tool, `rigtorp/udpreplay` (288★), is **34 kilobytes** and
dead since 2023. The only exchange-aware replayer found has **0 stars**.

Reference designs: TigerBeetle's VOPR (*"an entire cluster, running real code, subjected to
network, storage and process faults, at 1000× speed"*), FoundationDB's simulation-first
architecture, and `shadow/shadow` (1,712★, active — runs unmodified native binaries under
simulated time with 150+ syscalls intercepted; arguably repurposable rather than rebuilt).

### Why the three together are more than the sum

The mock exchange generates the feed. The fault injector corrupts it deterministically. The
recovery layer must survive it. The whole thing replays from a seed. That is TigerBeetle's VOPR
applied to exchange connectivity — and it runs entirely on a cloud VM.

### The falsifiable claim this produces

> *"87 of 247 open-source matching engines already fail a correctness oracle. Nobody has ever
> tested the **recovery** path, because there was no tool to test it with. Here is the tool.
> Here are N published ITCH feed handlers, and here is the deterministic fault schedule that
> makes each of them silently produce a wrong book."*

That is precisely the flash1-dev methodology applied to the one layer they did not test — and
that study got 181 issues filed and 18 fixed upstream. It is proof the shape of the artifact
lands with maintainers, which is the closest available proxy for landing with interviewers.

### Optional add-on if you want wire-level depth

**Gap #8 — an AF_XDP multicast market-data receiver.** IGMP coexistence alongside an XDP
redirect program, per-channel XSK steering via `XDP_REDIRECT` + BPF maps, `XDP_METADATA`
hardware RX timestamps, gap detection. `AF_XDP multicast` → **0 repos**; `AF_XDP market data` →
one 0★, 19 KB repo.

Crucially, this is *"the only item you can build and CI-test on commodity hardware"* — `veth` +
`XDP_SKB`, no Solarflare NIC required. It is the one kernel-bypass-adjacent project that
survives the cloud-only constraint. Trap to avoid: the AF_XDP socket API was **removed from
libbpf** into libxdp, and nearly every tutorial and 0-star repo still `#include <bpf/xsk.h>`.

### Free side-artifact, ~1 day

Publish the `MPMCQueue` position-independence finding from §2 as a short post with a 20-line
reproducer. It is already verified. It is not a project, but it is free credibility and a
conversation opener, and it demonstrates the habit of checking claims.

---

## 6. Cross-cutting: the write-up is the asset, not the repo

- The one detailed success story in the corpus — 4 years of unrelated cloud/front-end experience
  → interviews at multiple major firms — hinged on: *"I made logbook entries as I worked on the
  projects, and uploaded everything to the 2 github repos."*
- patio11's advice is the same: publish the deep technical write-up, use it for cold outreach.
- Every latency or correctness claim in public triggers a **methodology** challenge, never an
  implementation challenge. Pre-empt it: disclosed method, disclosed limits, one command to
  reproduce, seed included.
- Nine of ~a dozen CppCon sponsors are trading firms. Optiver, IMC, Maven, Tower, IEX, Crabel,
  Eisler, Lime and Citadel Securities all send speakers. A lightning talk or a well-written post
  about a real finding is a more direct line in than any resume bullet.
- Artifacts that travel in that channel look like Rigtorp's 136-star `hiccups`, **not** like
  anything with "ultra-low-latency trading engine" in the title.
- HRT, on what they screen for: *"truly understanding something inside and out"*; *"we rate our
  engineers as much on communication as we do on technical ability."* Notably, their engineering
  interview post never mentions portfolio repos at all — which is why the *write-up* and the
  *finding* matter more than the artifact.

---

## 7. Honest caveats

- **This thesis trades nanoseconds for correctness.** Given cloud-only hardware that trade is
  forced, and per §4 it lands in the higher-status half of the field. But if your target is
  specifically a hot-path optimization role, you will eventually need bare metal. The recommended
  acquisition, when you want it: a used Intel Skylake+ workstation with BIOS access and a
  Solarflare/Xilinx X2522 (ef_vi headers are BSD-2). Not needed for any of §5.
- Some gaps are unfixed because they are **physically unfixable** (SMIs, the PCIe floor, frame
  serialization, no-ground-truth-for-profilers), not because nobody tried.
- The "no open source tool does X" claim is mostly **inferred from in-house builds, not quoted**.
  The honest framing: firms build their own, repeatedly, and publish that they did.
- **Data moat to plan around:** NASDAQ publishes free ITCH sample pcaps (`emi.nasdaq.com`); CME
  does **not** publish free MDP 3.0 sample data. That single fact explains the 186-vs-8 repo
  asymmetry, and it means Component 1 should target ITCH/OUCH first.
- Research sourcing limits: X/Twitter returned HTTP 402 throughout (zero quotes, none invented).
  nuclearphynance.com is dead; wilmott.com contains essentially zero low-latency engineering
  discussion; r/hft is an interview-prep graveyard — r/highfreqtrading carries the signal. Reddit
  required a mirror. The WebSearch budget (200 calls) was exhausted before verification, so late
  checks were done via direct fetches and the GitHub API.
- One subagent claim I corrected on inspection: `microsoft/cpp-systematic-testing` was reported
  as archived 2026-06-11. It is archived, but its last push was **2022-09-29** — dead four years,
  2 commits ever.

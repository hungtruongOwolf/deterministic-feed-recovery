# Design

Mechanism choices for `dfr`, each with the real project and file that proves it works. Every
path below was read on 2026-07-30. Nothing here is inferred from memory.

Hard requirements this document serves: fully deterministic (no wall clock, no unseeded
randomness, no pointer-derived ordering), single-threaded core, no allocation in steady state,
and embeddable as a library.

---

## 0. Why a new library at all

Two open-source C++ MoldUDP64/ITCH libraries exist. Both fail the requirements above in specific,
citable ways, and reading them is the justification for this project.

**`penberg/helix` — unchecked flyweight over untrusted input.**
`moldudp64_session::process_packet` validates the 20-byte header
(`include/helix/nasdaq/moldudp64.hh:88-90`), then loops
`for (int i = 0; i < be16toh(header->MessageCount); i++)` doing
`reinterpret_cast<const moldudp64_message_block*>(p)` and `packet_view{p, message_length}`
**with no check that `p` or `p + message_length` is inside the datagram** (`:106-115`). A hostile
or merely faulty `MessageCount`/`MessageLength` walks off the buffer. It also throws from the
decode path (`:89`, `:131`).

This is exactly the bug class `dfr::chaos` exists to find. **"Truncated block", "MessageCount
overstated" and "MessageLength overruns datagram" become first-class fault ops.**

Worth keeping from it: `template<typename Handler>` for the inner decode loop, a span-like
non-owning `packet_view` (`include/helix/net.hh:10-39`), and virtual dispatch confined to the
`protocol`/`session` factory edge (`include/helix/helix.hh:93-123`).

**`bbalouki/itchcpp` — allocation in the steady state.**
`SequenceTracker::observe(std::string_view session, …)` constructs `const std::string key{session};`
**on every packet** to index an `unordered_map<std::string, uint64_t>`
(`include/itch/transport/sequencing.hpp:86-96,162`); `expected_next()` repeats it (`:138`). Gap
notification is a `std::function` plus a `virtual request_retransmit` (`:30-60,75-76`). Nicer API
than helix otherwise — `std::span<const std::byte>` in, `std::optional<Header>` out, `[[nodiscard]]`
throughout — but one heap allocation and two indirect calls per packet.

Its scope sentence is right, though, and we should copy it: *"The library never performs network
I/O itself: when `SequenceTracker` sees a gap it calls `request_retransmit`, and the caller is
responsible for issuing the actual re-request."*

**Consequence for us:** `dfr::recovery::gap_tracker` is a fixed-capacity array of
`{channel_id, expected_seq}` indexed by a small integer assigned at configuration time. No map, no
string, no `std::function`, `static_assert` on the channel count.

---

## 1. Decision table

| Part of `dfr` | Mechanism | Proof (file path) | Do **not** |
|---|---|---|---|
| `dfr::chaos` fault ops (drop / delay / dup / reorder / corrupt / truncate / line-down / gap) | `enum class fault_op : uint8_t` + `switch`; faults are POD records in a ring | re2 `enum InstOp` (`re2/prog.h:32-40`) dispatched by `switch (ip->opcode())` (`re2/nfa.cc:241,352,541`); opcode packed into 3 bits (`prog.h:160`) | virtual `IFaultStrategy::apply()` |
| One user-supplied custom fault | `struct { void* ctx; bool (*fn)(void*, packet_view&, rng&); }` | `RE2::Arg` (`re2/re2.h:883-947`); `fmt::detail::custom_value` (`fmt/base.h:2128-2132`); `llvm::function_ref` (`ADT/STLFunctionalExtras.h:40-52`); `mi_register_output(fn, void* arg)` (`mimalloc.h:147-151`) | `std::function` in the pipeline |
| Transport decode (MoldUDP64, IEX-TP, in-memory sim) | template policy param + C++20 concept, monomorphized | helix `template<typename Handler> class moldudp64_session` (`moldudp64.hh:30-52`); Aeron `template<typename F> int poll(F&&, int)` (`Subscription.h:206-229`, tag 1.44.1) | virtual in the per-message loop |
| Runtime protocol choice from CLI | exactly **one** virtual boundary, at the app edge, on a data-free interface | helix `class protocol { virtual session* new_session(void*) = 0; }` (`helix.hh:118-123`); quill `Sink::write_log` pure-virtual but backend-only (`quill/sinks/Sink.h:138-143`); Core Guidelines **I.25** | pushing it into decode |
| Buffer access | flyweight over `span<const std::byte>` + one `bounds_check()` choke point, no-op'd by macro | Aeron `AtomicBuffer::boundsCheck` with `#if !defined(DISABLE_BOUNDS_CHECKS)` and a same-signature no-op `#else` (`concurrent/AtomicBuffer.h:457-471`); `overlayStruct<T>` (`:189-208`) | raw `reinterpret_cast` — see helix above |
| Bad wire data (**expected input**) | `enum class error : uint8_t` + `[[nodiscard]] dfr::result<T>`, with `get(T&)` out-param form | simdjson `error_code` (`error.h:19-54`), `simdjson_result<T>` (`error.h:279-383`), `simdjson_warn_unused` everywhere, `SIMDJSON_EXCEPTIONS=0` | exceptions in the core |
| Programmer error | assert, enabled **independently of `NDEBUG`** | quill `QUILL_ASSERT` gated on `QUILL_ENABLE_ASSERTIONS \|\| !NDEBUG` (`quill/core/Common.h:21-36`); libc++ `_LIBCPP_HARDENING_MODE_FAST`; `ABSL_ASSUME` (`absl/base/optimization.h:268-284`) | asserts that vanish in release |
| Hot-path config (ring sizes, max blocks/packet) | traits type: `struct opts { static constexpr … }` as template param | quill `FrontendOptions` (`quill/core/FrontendOptions.h:38-76`) consumed via `std::conditional_t` (`quill/Logger.h:65-70`) | macros |
| Cold config (`dfr::venue` setup, reporting) | aggregate struct, default member inits, designated initializers | quill `BackendOptions`; `RE2::Options` (`re2/re2.h:618-745`); Abseil TotW #173 | builder chains |
| Message-type / codec dispatch | **constrained CPO object → member-function hook via `if constexpr` → default** | C++26 `[exec.connect]`: *"`connect(sndr, rcvr)` is expression-equivalent to `new_sndr.connect(rcvr)` if that expression is well-formed … Otherwise, `connect-awaitable(...)`"*. For foreign types: ADL + poison pill (libstdc++ `bits/ranges_base.h:105-138`) or specialization (quill `Codec<Arg>`, `quill/core/Codec.h:166-167`) | **`tag_invoke`** — never advanced past r0; zero occurrences in `[functional.syn]`/`[execution.syn]` |
| Clock | `template<typename Clock>` satisfying `std::chrono` Clock + `advance()` | seastar `manual_clock` (`core/manual_clock.hh:31-48`) with `template<typename Clock = steady_clock_type> class timer` (`core/timer.hh:79-80,178-186`) + `extern template` | virtual clock — quill `UserClockSource::now()` and FDB `INetwork::now()` both pay a vcall |
| Seeded RNG | own the engine **and** the range reduction | FDB `boost::random::mt19937_64` with the in-code reason *"to get consistent output across different compilers and therefore across different C++ standard library implementations"* (`flow/include/flow/DeterministicRandom.h:44-47`); hand-rolled `gen64() % range` (`DeterministicRandom.cpp:47-66`) | `std::uniform_int_distribution` (algorithm unspecified); `absl::BitGen` — its header says same-seed sequences *"need not"* match across processes (`absl/random/random.h:62-85`) |
| RNG API shape | two named accessors, so determinism is visible at the call site | FDB `deterministicRandom()` vs `nondeterministicRandom()` (`IRandom.h:220,224`) | one global `rng()` |
| Determinism debugging | optional per-draw trace for diffing two runs | FDB `extern FILE* randLog;` (`IRandom.h:211`) + `fprintf(randLog, "Rint %d\n", i)` (`DeterministicRandom.cpp:62-64`) | — |
| Fault vocabulary | paired, **undoable** verbs | FDB `ISimulator` `clogInterface`/`clogPair`/`unclogPair`, `disconnectPair`/`reconnectPair` (`fdbrpc/simulator.h:301-315`), `enum ClogMode` (`:46`) | a fault you cannot undo — it is untestable |
| Per-call handlers | `function_ref`-shaped: two words, non-owning | `llvm::function_ref` (`ADT/STLFunctionalExtras.h:40-52`), `LLVM_GSL_POINTER` + `LLVM_LIFETIME_BOUND` | `std::function` |
| Reporting worker | drivable **manually** from the test loop | quill `ManualBackendWorker::init/poll/shutdown` (`quill/backend/ManualBackendWorker.h:30-90`), forces `sleep_duration = 0ns` | thread-only backend |
| Type erasure across a queue | monomorphized function pointer, no vtable, no RTTI | quill `using FormatArgsDecoder = void (*)(std::byte*&, DynamicFormatArgStore&); template<class...Args> inline constexpr FormatArgsDecoder decoder_ptr = &decode_and_store_args<remove_cvref_t<Args>...>;` (`quill/core/Codec.h:414-423`) | `std::any`, `dynamic_cast` |
| Public variadic API | one-line `inline` wrapper that erases args and calls a **non-template** function | fmt `format(format_string<T...>, T&&...)` → `vformat(string_view, format_args)` (`fmt/format.h:4382-4397`) — O(1) template instantiation per call site | a variadic template that does real work |

---

## 2. Error handling — three tiers, decided by *who made the mistake*

**Tier 1 — programmer error → assert, and ship the asserts.** TIGER_STYLE is the governing text:

> Assertions detect programmer errors. Unlike operating errors, which are expected and which must
> be handled, assertion failures are unexpected. The only correct way to handle corrupt code is to
> crash. Assertions downgrade catastrophic correctness bugs into liveness bugs.

Give `dfr` three levels — off / fast (bounds + frame invariants) / paranoid (full pair-assertion
density) — defaulting to **fast in release**. Precedent: quill decouples assertions from `NDEBUG`;
libc++ `_LIBCPP_HARDENING_MODE_FAST` docs say *"contains a set of security-critical checks that can
be done with relatively little overhead in constant time"* and *"We recommend most projects adopt
this"*; Aeron ships a same-signature no-op `boundsCheck`.

**Tier 2 — malformed wire input → a value, never an exception.** For this project a corrupt packet
*is* the product. Copy simdjson exactly: `SUCCESS = 0` so `if (error)` reads correctly, plus
`is_fatal(error)` separating recoverable from stream-invalidating. `gap_detected` is not fatal;
`session_id_changed_mid_stream` is. `get(T&)` is the recommended idiom because it branches once.

**Tier 3 — setup/config errors → may throw, behind a macro.** `dfr::venue`'s CLI may throw;
`dfr::chaos` and `dfr::recovery` may not.

**Write our own `dfr::result<T>`; do not vendor tl::expected.** `std::expected` is C++23 and out of
reach. tl::expected is 2,475 lines of pre-concepts SFINAE and still cannot give us simdjson's
chain-by-specialization. Borrow two things: `[[nodiscard]]` on the **class** rather than per
function (`tl/expected.hpp:1277`), and the split where `value()` checks and throws while
`operator*`/`error()` only assert (`:2015-2059`). Name the monadic ops exactly as C++23 does —
`and_then`, `transform`, `or_else`, `transform_error`, `error_or` — so migrating later is a typedef.

**Why no exceptions in the core, concretely:** they make the fault path a different control-flow
path from the success path, which is the one thing a deterministic tool cannot tolerate.

---

## 3. TIGER_STYLE rules adopted

From `tigerbeetle/docs/TIGER_STYLE.md`, verbatim where quoted.

**Limits**
- *"**Put a limit on everything**… all loops and all queues must have a fixed upper bound to prevent
  infinite loops or tail latency spikes… Where a loop cannot terminate (e.g. an event loop), this
  must be asserted."* → the MoldUDP64 `MessageCount` loop gets a compile-time
  `max_blocks_per_packet` **and** an assert. This is precisely the helix bug.
- *"All memory must be statically allocated at startup. **No memory may be dynamically allocated
  (or freed and reallocated) after initialization.**"*
- *"Use explicitly-sized types like `u32` for everything."*

**Assertions**
- *"**Assert all function arguments and return values, pre/postconditions and invariants**… The
  assertion density of the code must average a minimum of two assertions per function."*
- *"For every property you want to enforce, try to find at least two different code paths where an
  assertion can be added."* → assert the frame invariant on **encode and on decode**. This one rule
  is what makes a fault injector trustworthy.
- *"**The golden rule of assertions is to assert the _positive space_ that you do expect AND to
  assert the _negative space_ that you do not expect** because where data moves across the
  valid/invalid boundary between these spaces is where interesting bugs are often found."*
- *"prefer `assert(a); assert(b);` over `assert(a and b)`"*; *"Use single-line `if` to assert an
  implication: `if (a) assert(b)`"*; *"**Assert the relationships of compile-time constants**"*.
- *"you may use a blatantly true assertion instead of a comment as stronger documentation where the
  assertion condition is critical and surprising."*

**Control flow**
- *"**hard limit of 70 lines per function**"*; *"**Centralize control flow.** When splitting a large
  function, try to keep all switch/if statements in the 'parent' function… **Keep leaf functions
  pure.**"* — independently, this is also the mitigation for the switch-proliferation hazard in §6.
- *"Use **only very simple, explicit control flow**… **Do not use recursion**."*
- *"don't do things directly in reaction to external events. Instead, your program should run at its
  own pace"* → `dfr::recovery` is `poll()`-driven, never socket-callback-driven.
- *"`void` trumps `bool`, `bool` trumps `u64`, `u64` trumps `?u64`"* — dimensionality is viral up
  the call chain.
- *"Negations are not easy! State invariants positively."*
- *"**Explicitly pass options to library functions at the call site, instead of relying on the
  defaults.**"*
- *"Be on your guard for **buffer bleeds**… where a buffer is not fully utilized, with padding not
  zeroed correctly."* → unzeroed padding in a `dfr::venue` packet breaks byte-for-byte
  reproducibility. Zero it, and assert it.

**Naming**
- `snake_case` for functions, variables, files. Acronym caps in types (`VSRState`, not `VsrState`).
- *"Add units or qualifiers to variable names, and put the units or qualifiers last, sorted by
  descending significance"* → `latency_ns_max`, not `max_latency_ns`, so `latency_ns_min` lines up.
- *"prefix the name of the helper function with the name of the calling function"* →
  `read_sector()` / `read_sector_callback()`. *"Callbacks go last in the list of parameters."*
- *"Do not abbreviate variable names."* Prefer nouns to participles: `replica.pipeline` beats
  `replica.preparing`.
- *"A function taking two `u64` must use an options struct."*
- *"**The usual suspects for off-by-one errors are casual interactions between an `index`, a `count`
  or a `size`.**"*

---

## 4. Layout

```
include/dfr/
  core/        packet_view, result<T>, error, assert, attributes, rng, clock
  wire/        moldudp64.hpp, iextp.hpp, flyweights, bounds_check
  chaos/       fault_op, schedule, injector<Transport, Clock, Rng>
  recovery/    gap_tracker, retransmit, snapshot, ab_arbiter
  venue/       mock exchange, in-memory transport
  detail/      everything else
src/dfr.cc     # single optional TU
```

- **Header-only by default, one compiled TU optional.** fmt's mechanism: out-of-line bodies in
  `*-inl.hpp` prefixed `DFR_FUNC`, which is `inline` under `DFR_HEADER_ONLY` and empty otherwise
  (`fmt/format.h:4432-4435`), compiled once by a small `src/`. quill uses the same shape.
- **`inline namespace v1`** so the real symbol is `dfr::v1::injector` (quill: `inline namespace v12`).
- **`DFR_BEGIN_EXPORT`/`END_EXPORT` markers** so a C++20 module build stays possible; quill's
  `src/quill.cc` is already `export module quill;`.
- **A single-header amalgamation** for drop-in embedding (simdjson `SIMDJSON_SINGLEHEADER`).
- **No PIMPL in the core.** It defeats inlining, adds an indirection per call, and forces
  allocation — three of the four hard requirements. Use it only in `dfr::venue`'s socket layer,
  where a syscall dominates anyway.
- **Hide with `detail::` + `private` + `friend`**, not with PIMPL. quill:
  `friend class detail::BackendWorker;`. Aeron and atomic_queue: `friend Base;`.

---

## 5. API conventions

**Parameter passing.** Views by value — TotW #1: *"You should pass `string_view` by value just like
you would pass an int or a double."* TotW #93: *"It is usually better to pass `Span` by value when
used as a function parameter."* Core Guidelines **F.16** enforcement threshold: warn above
`4 * sizeof(void*)`.

**Out-params are legitimate here**, on **F.20**'s own exception: *"To reuse an object that carries
capacity… across multiple calls to the function in an inner loop: treat it as an in/out parameter
and pass by reference."* Hence `decode(span, header& out)`. Rule: **all inputs before all outputs**,
and never append a parameter just because it is new.

**Implicit → const view, explicit → mutable view.** `Span<const T>` converts implicitly from any
container; `Span<T>` requires an explicit constructor (`absl/types/span.h:123-128,236-252`). Adopt
for `dfr::packet_view` / `dfr::mutable_packet_view`.

**The lifetime triple — highest-value single change to a zero-copy API.**
`[[gsl::Pointer]]` on views, `[[gsl::Owner]]` on owners, `[[clang::lifetimebound]]` on retained
parameters (`absl/base/attributes.h:979,1010,901-924`). Use both this *and* simdjson's
`=delete`-the-rvalue trick (`ondemand/parser.h:138`), despite Abseil's TotW #149 caution that
*"the C++ type system is simply not capable of encoding the necessary details about lifespan
requirements."*

**No `bool` parameters** — TotW #94: *"the argument at the callsite is very often a literal `true`
or `false`, and that gives the reader no contextual cues."* So `inject(pkt, drop_policy::drop)`.

**No `shared_ptr` parameters** — **F.7**: *"Passing a shared smart pointer… implies a run-time
cost."* **R.31** calls passing one where sharedness is unused *"a silent pessimization."*

**Uniform across exemplars, so adopt without debate:**
- `[[nodiscard]]` on every fallible or query function (simdjson, mimalloc, rigtorp, quill all do;
  atomic_queue does not, and is worse for it).
- `explicit` on every one-arg constructor and on `operator bool`.
- **Non-copyable by default** for anything owning a queue or buffer — quill `LoggerImpl`/`Sink`,
  `RE2` (`re2.h:296-304`), both rigtorp queues, Aeron `FragmentAssembler`.
- Layout invariants as `static_assert` **with the reason in the message** — rigtorp `MPMCQueue.h:140-152`:
  *"head and tail must be a cache line apart to prevent false sharing"*.
- A virtual destructor on **only** the one `dfr::venue::protocol` interface (**C.35**).
- `= delete` as an active design tool, not just for copies: atomic_queue's
  `template<class T> T as_signed(T) = delete;` (`defs.h:184-185`), fmt's `formatter() = delete` as a
  disabled-marker (`base.h:639-644`).
- Skip `ABSL_MUST_USE_RESULT` — Abseil's own header says prefer `[[nodiscard]]` on C++17 and up.
  Same for `ABSL_CACHELINE_ALIGNED` → `alignas(std::hardware_destructive_interference_size)` on
  individual members, expecting to special-case Apple (rigtorp `MPMCQueue.h:43`).

**Restraint, from Abseil's own headers:** *"annotating every branch in a codebase is likely
counterproductive"* (`optimization.h:183`); on cacheline alignment, *"Prefer applying this attribute
to individual variables. Avoid applying it to types"* and *"It is easy to use this attribute
incorrectly, even to the point of causing bugs that are difficult to diagnose"* (`:117-157`).

**Naming: all `snake_case`, types included.** `dfr::packet_view`, `dfr::result<T>`,
`dfr::fault_op`, `dfr::chaos::injector`, `dfr::recovery::gap_tracker`.

There is no consensus among the exemplars — simdjson and fmt are all `snake_case`; quill,
atomic_queue and rigtorp are `PascalCase` types; Aeron is Java-flavoured. The argument is not taste.
Core Guidelines **NL.10**: *"Prefer `underscore_style` names… the original C and C++ style and used
in the C++ Standard Library."* And empirically: fmt spells everything like the standard library and
was adopted into C++20 as `std::format` essentially verbatim. If `dfr`'s types are meant to be
vocabulary types inside someone else's codebase, spelling them like `std::span` is what makes them
feel native.

Then: `detail::` for internals, `trailing_` for private members, `ALL_CAPS` for macros only,
units-last (`latency_ns_max`), and **Aeron's lesson that memory ordering belongs in the method
name** — `load_acquire`, `store_release` — so it is greppable.

---

## 6. Enum + switch vs virtual dispatch — and the hazard

Read the primary source: `unclebob/cmuratori-discussion` (`cleancodeqa.md`, `cleancodeqa-2.md`).

**Muratori's numbers:** virtual dispatch ≈35 cycles/shape → `switch` 24 → lookup table 3.0–3.5
(≈10×) → AVX ~20–25×. *These are article-only; his benchmark source is unpublished, so treat them
as unreproduced.*

**Where he is right, and it is our case exactly** (`cleancodeqa-2.md:62`): *"enums/flags and
if/switches are much better than classes for this design… better along every axis. They will be
faster, easier to maintain, easier to read, easier to write, easier to debug."* And the
data-oriented point that *is* our architecture: *"anything that can be turned into data writes
should be, and function calls should be minimized… The Linux kernel design of io_uring looks like
my design!"* → faults are records a single-threaded loop drains, not a chain of virtual `apply()`.

**The real hazard, and Martin states it correctly** (`:107-134`): with `if/switch` at the interface,
adding a device means editing five switches in five files, and *"It is not at all uncommon to see
switch statement scattered throughout the body of the code — all with the same cases but with
different targets. There can be a lot more than five; and they reproduce like gerbils."*

**Mitigation:** centralize every switch in one file — which TIGER_STYLE's *"Centralize control
flow"* already requires.

**Both concede, and the concession settles it for us.** Martin (`:136-138`): *"there is a time and
place for both… **But then I don't work in constrained environments. Memory and cycles don't mean a
lot to me anymore.** What matters most to me is source file organization."* That stated criterion is
the exact inverse of ours. Our fault ops and transports are a closed, small, slowly-changing set we
own.

Also worth recording, because it is the fair reading of the wider genre: patterns are a
*vocabulary*, and Strategy-as-enum, Flyweight-as-Aeron-`Flyweight` and Visitor-as-`switch` are all
patterns correctly implemented without inheritance. The defensible core of the criticism is Acton's
*"Solving problems you probably don't have creates more problems you definitely do"* — and the part
usually dropped when that talk is cited is that his slide listing the cost includes **"POOR
STABILITY / POOR TESTABILITY"**, not only speed. That is the argument that applies here.

**SOLID, specifically.** Open-Closed is where SOLID and C++ genuinely conflict: extension via
inheritance costs a vtable per call; via specialization or a CPO it costs nothing but must happen at
compile time. Liskov and Dependency-Inversion survive intact if "interface" reads as **concept**
rather than abstract base class — which is what simdjson's `concept deserializable`
(`generic/ondemand/deserialize.h:33`) and foonathan's overload-ranking
`full_concept : min_concept : std_concept : error` ladder (`allocator_traits.hpp:71-92`) actually
implement.

---

## 7. Two mechanisms worth singling out

**seastar's `manual_clock` is zero-cost simulated time, and it is strictly better than the
alternatives.** `manual_clock` supplies `rep/period/duration/time_point/now()` plus
`static void advance(duration)` (`core/manual_clock.hh:31-46`); `template<typename Clock = steady_clock_type>
class timer` calls `Clock::now()` **statically** (`core/timer.hh:79-80,178-186`), with
`extern template` declarations to control bloat. By contrast quill's `UserClockSource::now()` and
FoundationDB's `INetwork::now()` are both virtual and pay a call per query.

**foonathan/memory shows how to make "no allocation" *provable* rather than hoped-for.** Every pool
exposes `static constexpr std::size_t min_block_size(node_size, number_of_nodes)`
(`memory_pool.hpp:65-70`) plus `capacity_left()` and `next_capacity()`. Give every `dfr` ring and
arena a `static constexpr bytes_required(...)` and assert `capacity_left()` is unchanged across a
steady-state cycle. That turns a claim into a test.

---

## 7b. Composition decisions in `dfr::recovery`

Three choices that only make sense once the components are wired together, recorded here
rather than in `client.hpp` so that file stays about the state machine.

**One client per channel, not per venue and not per process.** Recovery state, retransmit
servers and snapshot facilities are all per-channel at the venues modelled, and a
multi-channel client would multiply a 64 KiB replay buffer by the channel count to no
purpose. `gap_tracker` stays multi-channel anyway, because a tool that only wants to *watch*
many channels — `tools/inspect` — has no use for the rest of the machinery.

**The replay buffer is message-granular while sequencing is packet-granular.** A snapshot's
resume point can land in the middle of a buffered packet. At packet granularity the client
would then have to replay the whole packet, duplicating messages the snapshot already
accounts for, or drop it and lose the tail. Neither is acceptable, so the buffer indexes
messages — which is why handing them over is a separate call the caller makes
(`buffer_message`): splitting a packet into messages needs the wire cursor, and the wire
layer is deliberately not a dependency of recovery.

**Two positions, kept in step explicitly.** The arbiter's watermark means "the highest
sequence the merged stream has reached"; the client's `delivered_through()` means "the
highest sequence handed downstream". They are equal while live and diverge while recovering,
because messages held for replay have been seen and not delivered. Separately, a heartbeat
advances the *tracker's* expectation while delivering nothing, so it cannot advance the
arbiter's watermark — and on IEX two thirds of packets are heartbeats. The client therefore
calls `arbiter::adopt(tracker.expected_sequence())` after every observation, which makes the
disjointness of "newly arrived" and "newly repaired" a theorem instead of a coincidence.
Both of these were found by integration testing; neither was reachable from the unit tests.

---

## 8. Corrections to things commonly said

- **`atomic_queue` has no `if constexpr` and no `[[nodiscard]]` anywhere** (checked `master` and
  `v1.6.5`). Its knobs are plain `if` on `static constexpr bool` (`:297,329`) — deliberately, so
  both arms compile for every instantiation. And `TOTAL_ORDER` is not a branch at all, it selects a
  value: `constexpr auto memory_order = Derived::total_order_ ? seq_cst : relaxed;` (`:438`). Copy
  that shape.
- **`tag_invoke` never advanced past r0** and appears zero times in C++26's `[functional.syn]` /
  `[execution.syn]`. C++26 kept the CPO façade but made the hook a **member function**. P2855R1's own
  justification: *"The definition of customization points is much simpler, to a ridiculous extent."*
- **Aeron's hand-written C++ client is gone from `master`**, replaced by
  `aeron-client/src/main/cpp_wrapper/` — a header-only façade over the C library preserving the same
  API via a `void* clientd` + `template<typename H> static void doPoll(...)` trampoline
  (`cpp_wrapper/Image.h:80-87`). Cited paths above are at tag **1.44.1**. If `dfr` ever needs a C
  ABI, that is the pattern.
- **Acton's talk contains no "90% of transistors" claim and no L2-latency-in-nanoseconds figure** —
  zero grep hits across all 201 slides and the full transcript. Commonly misattributed, probably
  conflated with Tony Albrecht's *Pitfalls of Object Oriented Programming*.
- **Sean Parent and Muratori/Acton are orthogonal critiques of inheritance, routinely conflated.**
  Parent fixes ownership and coupling (*"a shared pointer is as good as a global variable"*);
  Muratori and Acton fix memory layout. This project needs both fixes, and they point the same way:
  **POD records in a ring, no shared ownership, no vtable in the loop.**

---

## 9. Sources

Read directly, 2026-07-30: quill · simdjson · fmt · atomic_queue · rigtorp SPSCQueue/MPMCQueue ·
Aeron (tag 1.44.1) · Abseil (`span.h`, `attributes.h`, `optimization.h`, `random.h`, TotW #1/#93/#94/#149/#173) ·
re2 · LLVM `STLFunctionalExtras.h` · mimalloc · foonathan/memory · seastar · FoundationDB
(`flow`, `fdbrpc`) · tl::expected · libc++ hardening docs · C++ Core Guidelines (F.7, F.16, F.20,
I.25, C.35, C.120, R.31, NL.8/9/10) · Google C++ Style Guide · `tigerbeetle/docs/TIGER_STYLE.md` ·
`unclebob/cmuratori-discussion` · `penberg/helix` · `bbalouki/itchcpp`.

# Style

House rules for `deterministic-feed-recovery`. Every rule below is calibrated against code
measured in real projects, not asserted. Measurements were taken 2026-07-30.

---

## 0. The thing most style advice gets wrong

**"Comments say why, not what" is not what the exemplars do.** Linux
`Documentation/process/coding-style.rst` ch. 8 says the opposite, verbatim:

> NEVER try to explain HOW your code works in a comment: it's much better to write the code so
> that the **working** is obvious… Generally, you want your comments to tell WHAT your code
> does, not HOW.

Google's C++ style guide says a third thing (prefer *why*, or make the code self-describing).
These reconcile once you separate the axes. The rule the exemplars actually follow:

> **Comment the things a competent reader cannot recover from the code: the contract, the
> reason, the thing you ruled out, and the external fact.** The mechanism, the loop and the name
> are carried by the code.

**Comment density is not a virtue.** Measured:

| Project / file | Lines | Comment-only | % | Assertions |
|---|---|---|---|---|
| Linux `kernel/sched/fair.c` | 15,459 | 5,074 | 33% | 15 `lockdep_assert*`, 43 `WARN_ON` |
| SQLite `src/btree.c` | 11,633 | 3,647 | 31% | 735 `assert()` |
| TigerBeetle `src/vsr/replica.zig` | 12,413 | 1,897 | 15% | 1,582 `assert()` + 57 `maybe()` |
| quill `core/BoundedSPSCQueue.h` | 475 | 73 | 15% | 2 `static_assert` |
| `max0x7ba/atomic_queue.h` | 770 | 46 | 6% | ~5 |
| `rigtorp/SPSCQueue.h` | 237 | **7** | **3%** | 9 |

A 3% file and a 33% file are both exemplary. What differs is *what* is commented.

---

## 1. Comments

**1.1 Contract at the declaration, reason at the code.** LLVM's rule: don't duplicate the doc
comment in header and source. Public contract goes in the header; the *why* goes next to the
code that needed a decision.

**1.2 No Doxygen tags.** Prose, in house shape. The census is genuinely split — quill and
simdjson run full Doxygen (261 `@param` in quill); **Abseil, folly, googletest and Google's own
style guide use none** (`grep -ci doxygen` on the rendered Google C++ guide = 0). fmt keeps
Doxygen only as a parser and writes plain `///` prose. For a solo library, tags are slots that
invite filler. Write sentences.

**1.3 Every function that has thread-affinity or blocking behaviour gets a `Context:` line.**
Borrowed from the kernel. Example:

```cpp
/// Advance the recovery state machine.
///
/// Context: injector thread only. Never blocks. Never allocates after construction.
void step(Clock::time_point now);
```

**1.4 Prefer an assertion to a comment.** A comment goes stale silently; an assertion cannot.

**1.5 Assert the negative space too.** Adopt TigerBeetle's `maybe()` — a two-line helper
asserting that a condition is *permitted*, used where you would otherwise write a hedging
comment about a reachable state.

**1.6 Memory ordering: name the partner, name the invariant, name what you ruled out.**
`// acquire to ensure visibility` is worthless. This is the single most unfakeable comment genre
in the codebase, so it is where to spend effort.

**1.7 Wire formats: violate "why not what" deliberately.** Byte offsets and field layouts get
explicit `what` comments, and every wire struct gets `static_assert` on `sizeof` **and**
`offsetof` for each field. Neither Aeron, libtrading nor SBE-generated code does this — doing it
is cheap and visibly better than the references.

**1.8 State the guarantee you do *not* provide.** In the first person if that is what it takes.
`lib/rbtree.c` in Linux says *"Nor did I check for loops involving parent pointers"*.

**1.9 TODOs: rare, scoped, honest.** TigerBeetle runs 1 per 566 code lines. That is the ceiling.

---

## 1.10 File size and seams

**One header, one concept. Target 200 lines; treat 300 as a smell and 400 as a
defect.**

Not an arbitrary limit — the reason is the same one TIGER_STYLE gives for its
70-line function rule: *"There's a sharp discontinuity between a function fitting
on a screen, and having to scroll."* The same discontinuity applies to finding
the thing you came for in a file. A 600-line header means the reader scrolls past
four unrelated concepts to reach the fifth, and a reviewer cannot tell which
parts of a diff belong together.

The split goes at a **seam**, never at a line count. A seam is a place where two
things could be understood, tested and changed independently:

- a wire protocol splits into constants / header / cursor / encoder, because a
  reader fixing a decode bug never needs the encoder
- a view type splits from its mutable twin, because the read path and the write
  path have different callers
- a cohesive vocabulary type does **not** split just because it is long.
  `result<T>` is 390 lines and stays one file: every line of it is the same
  concept, and separating the monadic operations from the class they belong to
  would make both halves harder to read.

Where a directory replaces a header, keep an umbrella header of the same name
that includes the parts. Existing `#include <dfr/wire/moldudp64.hpp>` must keep
working, and a caller who wants only the constants should be able to include only
those.

The same rule applies to tests, with the same seam: one test file per concept,
and shared fixtures in a `support/` header rather than copied.

---

## 2. Assertions

The discipline, with measured targets:

1. **Target 1 assertion per ~15 code lines, and ≥2 per non-trivial function.** TIGER_STYLE
   mandates a minimum of two per function; TigerBeetle measurably achieves **1.82** and
   **1 per 14.6 lines**; SQLite runs **1 per 23 lines** (6,754 asserts, their figure). Land in
   that band. Do not pad trivial accessors to hit a number.
2. **Split compound assertions.** `assert(a); assert(b);` beats `assert(a && b)` — more precise
   information on failure.
3. **`if (a) assert(b);`** for implications.
4. **Assert every property on two different code paths.** TIGER_STYLE: assert validity right
   before writing to disk *and* immediately after reading back. Here: assert frame invariants at
   parse **and** again at replay.
5. **`static_assert` every wire struct.** See 1.7.
6. **Attach a sentence to load-bearing assertions**: `assert(cond && "why this must hold")`.
7. **Three build modes, and publish the measured cost.** SQLite runs *"about three times slower
   when asserts are enabled"* and therefore ships with them off. We want: asserts on
   (dev + simulator), asserts off (release), and the measured delta in the README. "Asserts cost
   Nx on the parse path, so the shipped build disables them; the simulator always runs with them
   on" is exactly the method disclosure this project is graded on.
8. **Assertions are not a substitute for understanding.** TIGER_STYLE, verbatim, and it belongs
   in `CONTRIBUTING.md`:

   > Assertions are a safety net, not a substitute for human understanding. With simulation
   > testing, there is the temptation to trust the fuzzer. But a fuzzer can prove only the
   > presence of bugs, not their absence.

**C++26 contracts (`pre`/`post`/`contract_assert`) are not usable here.** `__cpp_contracts =
202502L` ships in **GCC 16 only** — no Clang, no MSVC, no Apple Clang. Use plain assertions.

---

## 3. README

Measured across 22 exemplars. Word counts: tigerbeetle **226**, hiccups **310**, shadow 466,
SPSCQueue 1,061, simdjson 1,413, quill 4,382, nlohmann/json 12,197.

**Length is bimodal and the respected-solo tier is short.** Aim for 400–900 words until there
are real results to report.

Section order:

```
# deterministic-feed-recovery        ← no adjective, no performance claim
one-sentence description
## Quickstart                        ← code within ~150 words of the top
## What it does / How it works       ← method in prose BEFORE any number
## Building
## Reproducing the results           ← exact commands, exact machine
## Results                           ← percentiles, dated, with environment pasted
## When not to use this              ← names a specific better tool
## Documentation
## License
```

---

## 4. The credibility moves that matter

Only **5 of 22** exemplars have a limitations section at all, which is why these are cheap
differentiation. Ranked:

| # | Move | Proven by |
|---|---|---|
| 1 | **Name a tool that beats yours, and at what** | hiccups points readers at Linux `osnoise`, which *"additionally shows you the sources of the jitter"* |
| 2 | Method fully explained before any number | hiccups gives the loop, the comparison and how the threshold is derived, before the table |
| 3 | Methodology as an executable script that also restores the environment | atomic_queue `scripts/benchmark.sh` has `prologue`/`epilogue` |
| 4 | **Publish a table where you lose** | quill sorts itself third behind XTR and NanoLog |
| 5 | Recommend a competitor for a named use case | quill: *"If you prefer a binary-log workflow, MS BinLog is a strong alternative"* |
| 6 | An explicit "why you shouldn't use this" | ripgrep: *"The best tool for this job is good old grep."* |
| 7 | Derive limitations from design choices | atomic_queue: *"These design choices are also limitations:"* |
| 8 | Paste the environment verbatim including `/proc/cmdline` | quill |
| 9 | State non-goals as first-class navigation | shadow `docs/security.md` = *"# Non-goal: Security"* |
| 10 | **Hedge the headline property grammatically** | shadow: *"it should produce deterministic results (it's a bug if it doesn't)"* |
| 11 | Date benchmarks and let them go stale honestly | mimalloc: *"Last update: 2021-01-30"* |
| 12 | Invalidate your own best-case numbers | atomic_queue: *"Always benchmark your specific thread placement"* |
| 13 | Admit bias | ripgrep: *"given that I am the author of one of the tools in the benchmark, they are therefore also biased"* |
| 14 | Report null results as null results | rigtorp: *"I found little difference in spinlock performance between the different atomic operations"* |
| 15 | Refuse an unearned win | ripgrep: a competitor *"doesn't actually beat rg here: it just gets so confused… that it gives up"* |
| 16 | State the epistemic limit of the whole method | Jepsen: *"we can prove the presence of bugs, but not their absence"* |
| 17 | Publish Future Work as gaps in *your* coverage | Jepsen |
| 18 | Explain the naive version first, in full compilable code | rigtorp's ring-buffer post |
| 19 | Cite the books and papers you learned from | atomic_queue `## Reading material` |
| 20 | Document a footgun in your own API at length | seastar `doc/lambda-coroutine-fiasco.md` |
| 21 | Commit raw data and the analysis pipeline | atomic_queue `html/results.js` + `scripts/stats.py` |
| 22 | Commit editable diagram source beside the render | quill `.drawio` + `.svg` |
| 23 | Disclose AI assistance, isolated and caveated | FoundationDB `design/AI-generated/README.md`: *"We have not reviewed this in detail."* |

Moves **1, 4, 5, 6, 9, 10** are the ones a competing candidate's repo almost never has.

---

## 5. Anti-pattern checklist — run before publishing

- [ ] Title has no adjective and no performance claim — **0 of 22** exemplars have one
- [ ] Zero `*_SUMMARY.md` / `*_REPORT.md` / `*_COMPLETE.md` / `*_ANALYSIS.md` — 0 of 22; the
      negative baselines carry 46, 50 and **1,154**
- [ ] Zero occurrences of: blazing, blazingly, ultra, insanely, world-class, cutting-edge,
      state-of-the-art, revolutionary — **1** occurrence across all 22 READMEs
- [ ] No emoji in section headers — 2 of 22, neither in the rigor tier
- [ ] ≤3 badges, each carrying information — hiccups has 1
- [ ] No feature list with ✅/⚡ — systems tier writes `## Design Principles` instead
- [ ] No Roadmap / Status / Coming Soon section — **0 of 22**
- [ ] Every number carries CPU model, kernel, compiler + version, exact flags, run count
- [ ] Every benchmark table has a date
- [ ] At least one published result where we lose or tie
- [ ] A "When not to use this" section naming a specific better tool
- [ ] The headline property is hedged, not asserted
- [ ] No chart a monospace table would do better — **0 charts** across rigtorp's posts and
      ripgrep's 25 benchmarks
- [ ] At most one mermaid diagram, and only a message-sequence one — tigerbeetle has exactly 1
      in the whole repo
- [ ] No diagram without prose saying the same thing
- [ ] No `CODE_OF_CONDUCT.md` — 4 of 21, all corporate-backed
- [ ] No Doxygen site — 0 of 3 solo exemplars; hiccups is 7 files total
- [ ] No ADR directory — 0 of 7 systems repos; use FoundationDB's `design/` + template instead

### The LLM tells, concretely — grep for these

1. **Uniform docblock density.** Every function commented, all comments the same length. Real
   codebases are spiky. If comment-to-function ratio is near 1.0 with low variance, that is the
   tell.
2. **Narration instead of justification.** `// Loop over the messages and process each one.`
   Test: delete it. If nothing is lost, it was narration.
3. **Tag scaffolding with no content.** `@param seq The sequence number.` — filled because the
   template had a slot. This is why §1.2 bans tags: no slots, no filler.
4. **Confident breadth, no numbers.** Compare against real performance comments in
   `atomic_queue.h`: *"At least +1.5% faster throughput benchmark relative to RemapXor."* and
   *"FIFO and total order on Intel regardless, as of 2019."* — a number, a comparison, or a date.
5. **Generic `memory_order` justification.** §1.6 is unfakeable; that is the point.
6. **No negative space.** Nothing says what does not work or was not checked. **Loudest signal.**
7. **Uniformly upbeat register, no hedging.** SQLite's commits distinguish `Fix for [bug]` from
   **`Possible fix for [bug]`**.
8. **Comments that were true of an earlier draft.** Grep every comment against adjacent code
   once, deliberately, before publishing.

If any prose here is model-drafted, disclose and isolate it. FoundationDB quarantines machine
-written docs in a directory literally named `design/AI-generated/`, with the note *"We have not
reviewed this in detail."*

---

## 6. Commits

**Nobody uses Conventional Commits.** Checked for `commitlint.config.js`, `.commitlintrc.*`,
`.gitlint`, `.cz.toml`, `.czrc` across tigerbeetle, simdjson, abseil, redis, quill, fmt and
sqlite: **zero commit-lint tooling in any of them.**

House rule: imperative subject under ~72 chars, blank line, then *why* — what was ruled out, and
what the reader should know that the diff does not show. Hedge when the fix is a guess, the way
SQLite does.

Never add `Co-Authored-By` or generated-with trailers.

---

## 7. Sources

Read directly, 2026-07-30: Linux `Documentation/process/coding-style.rst`, `kernel/sched/fair.c`,
`lib/rbtree.c` · SQLite `src/btree.c`, `sqlite.org/testing.html` · TigerBeetle
`docs/TIGER_STYLE.md`, `src/vsr/replica.zig`, `src/testing/cluster/state_checker.zig` · simdjson
`doc/ondemand_design.md`, `doc/performance.md` · quill `include/quill/core/BoundedSPSCQueue.h`,
`docs/Doxyfile.in` · `max0x7ba/atomic_queue.h`, `scripts/benchmark.sh` · `rigtorp/SPSCQueue.h`,
`rigtorp.se/ringbuffer/` ("Optimizing a ring buffer for throughput", 2021-12-13) ·
`absl/base/thread_annotations.h` · folly `folly/synchronization/` · FoundationDB
`flow/include/flow/CoroutinesImpl.h`, `design/AI-generated/README.md` · shadow `docs/limitations.md`,
`docs/testing_determinism.md`, `docs/security.md` · seastar `doc/lambda-coroutine-fiasco.md` ·
ripgrep, hyperfine, mimalloc, nlohmann/json, fmt, Catch2, google/benchmark, tokio, jepsen,
aeron READMEs · Google C++ Style Guide · LLVM Coding Standards.

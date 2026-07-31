// The defects, which are the strongest thing this project has and were invisible on the page.
//
// Measured before writing this file: 113 passages in the commit history describe a defect found and why it was
// hard to see. On the page, none of them appeared outside a collapsed fold. That is designing from the wrong
// end — from what is true about the code, arranged tidily, rather than from what somebody arriving actually
// wants.
//
// Who arrives, honestly: a hiring manager with thirty seconds, and an engineer with five minutes. Neither wants
// a lesson in MoldUDP64 recovery; that is what *I* find interesting. What an engineer wants is evidence of
// judgement, and the only real evidence of judgement is a hard bug found and correctly explained — including
// the ones where the bug was in my own reasoning.
//
// Every entry below is a real commit. The claim, the way it hid, and the thing that caught it. Most of them are
// mistakes I made and then found; that is the point rather than an embarrassment, because a portfolio of things
// that went right is a portfolio of things nobody checked.
//
// Including the ones where the mistake was in the checking. One entry here is a guard I wrote that reported success
// on the bug it existed to catch, which is the most useful kind of thing to have written down.

export interface Finding {
  /** What happened, short enough to scan. */
  readonly title: string;
  /** Why it was invisible — the part that makes it interesting rather than just a bug. */
  readonly hid: string;
  /** What actually caught it. Never "code review". */
  readonly caught: string;
  /** What it changed, or what it says about the tooling everybody trusts. */
  readonly matters: string;
  /** "concurrency", "benchmarking", "protocol", "integration" — for the tag. */
  readonly kind: string;
  readonly where: string;
}

const REPO = "https://github.com/hungtruongOwolf/deterministic-feed-recovery/blob/main";

export const FINDINGS: readonly Finding[] = [
  {
    title: "A correct feed handler, consumed correctly, still gives the wrong book",
    hid: "The invariant is that the book after loss and repair equals the book that lost nothing. It failed on the first run — same 600 messages, same update counts, same trades, different book — and nothing in the library was wrong.",
    caught:
      "Building an order book and comparing it against a clean replay. No sequence-number check can see it: every number arrived exactly once.",
    matters:
      "While a hole is open the client keeps delivering later messages — on purpose, because stalling on a gap turns one loss into an outage. So a repair arrives after higher sequence numbers, and an aggregated book is last-write-wins. A consumer must apply in sequence order, not arrival order, and nothing warns it.",
    kind: "the hardest one",
    where: `${REPO}/tests/integration/book_oracle_test.cpp`,
  },
  {
    title: "The flaky abort was in the test harness, and one run was never enough",
    hid: "A threaded test aborted intermittently with SIGABRT, no failing expression, and a Catch2 assertion about its own output redirect. It pointed at the lock-free code, where nothing was wrong. I had read \u201c720 tests passed\u201d and pushed.",
    caught:
      "Running it in a loop rather than once: it reproduced on the second run, so it had never been platform-specific. A REQUIRE on the consumer thread was racing the main thread \u2014 Catch2's result capture is single-threaded.",
    matters:
      "My first guard ran it 40 times and reported success on the bug planted back. The failure rate is 2% a run, so 40 runs is a coin flip \u2014 and a guard that is a coin flip is worse than none, because its pass gets believed. 400 runs is 99.97%, and six seconds.",
    kind: "verification",
    where: `${REPO}/scripts/hammer-concurrency.sh`,
  },
  {
    title: "Two documentation guards printed a tick while checking nothing",
    hid: "Both built a regular-expression alternation with `${words[*]// /|}`, which looks like it joins with pipes and does not: bash substitutes inside each element, then joins with a space. The pattern could never match, so both guards fell to the else branch and reported success.",
    caught:
      "Breaking the thing on purpose to watch the check fail \u2014 and watching it pass. That habit came from the previous finding, one commit earlier.",
    matters:
      "One had been guarding the README's namespace count for weeks. A check that cannot fail is worse than no check, because the repository reads as verified: I had been citing that tick. Every guard here now gets a planted defect before it is believed.",
    kind: "verification",
    where: `${REPO}/scripts/check-docs.sh`,
  },
  {
    title: "Three functions claimed constexpr and could not deliver it",
    hid: "All three build a string_view over std::byte, which needs a reinterpret_cast — forbidden in constant evaluation. Clang accepts them because nothing ever constant-evaluates them, so the invalid path is never instantiated.",
    caught:
      "Adding a GCC job. It diagnoses eagerly and is right. Over four rounds the same job also found an ABI dependency in a lock-free structure, a silently truncated buffer, and nine redundant casts.",
    matters:
      "The keyword was a claim the function could not honour: anyone using it in a constant expression got a hard error. I had already fixed this exact defect once elsewhere and missed three more — a habit of remembering does not scale, a compiler does.",
    kind: "portability",
    where: `${REPO}/docs/BENCHMARKS.md`,
  },
  {
    title: "ThreadSanitizer passes a lock-free ring that is wrong",
    hid: "Replace the ring's release/acquire with relaxed and it can publish an index before the record's bytes. TSan reports nothing: it does not model relaxed atomics precisely, so it cannot tell a sufficient ordering from an insufficient one.",
    caught: "A property test on arm64 — weakly ordered, so it actually reorders. It failed 12 runs out of 12. On x86-64 the same broken code would very likely pass.",
    matters: "The tool everyone reaches for cannot check the thing it is reached for. So the guard here is the property test, and the ledger marks the ordering claim not measurable rather than claiming a proof.",
    kind: "concurrency",
    where: `${REPO}/docs/CONCURRENCY.md`,
  },
  {
    title: "A 55× benchmark result that measured two things at once",
    hid: "Pricing the paranoid assertions by comparing the dev and bench presets. One is Debug and the other Release, so the comparison moved optimisation level and assertion level together and reported the sum as the cost of one.",
    caught: "Reading the number and not believing it. Rebuilt at a fixed -O3 with only the assertion level changing.",
    matters: "The real answer is the opposite of what 55× suggested: 3× on a header decode, and nothing measurable on the paths that dominate an ingest. So the bounds-checked build can ship.",
    kind: "benchmarking",
    where: `${REPO}/docs/BENCHMARKS.md`,
  },
  {
    title: "A username that could never fit its own field",
    hid: "The order-entry session defaulted to the username DFRUSER — seven characters. SoupBinTCP's Username field is six. Every login would have been refused forever, looking exactly like an authentication problem.",
    caught: "Writing the seam between two components that were each already tested and each correct. The wire layer enforced its field width properly; the session had no idea what the width was.",
    matters: "Neither component could have found it. The constructor now asserts the credentials fit, so a value that can never appear on the wire stops the run instead of producing a mystery.",
    kind: "integration",
    where: `${REPO}/include/dfr/venue/order_session.hpp`,
  },
  {
    title: "A second line that splits a hole instead of closing it",
    hid: "“Two lines mean fewer retransmit requests” seemed obviously true and I had asserted it. At one pattern the second line carried the middle of a 27-message hole, turning one gap into two of 9 and 15: more requests, fewer messages.",
    caught: "Sweeping 400 patterns through the WebAssembly build instead of testing the claim against the one recording that motivated it.",
    matters: "Request count and message count answer different questions, so the trace format grew a field for the second. The page now states it as a tendency with the counterexample, not a law.",
    kind: "measurement",
    where: `${REPO}/README.md#what-a-400-seed-sweep-actually-showed`,
  },
  {
    title: "A heartbeat that could never reveal a gap",
    hid: "The client returned early when a packet delivered no new messages. Reasonable-looking, and it meant a heartbeat could never expose a hole — while two thirds of the packets in a real IEX capture are heartbeats.",
    caught: "Running the pipeline against a real capture rather than a synthetic stream. No unit test in the suite could have produced the shape.",
    matters: "A feed that goes quiet after a loss is exactly when recovery matters, and this was the case where it would have sat silent.",
    kind: "real data",
    where: `${REPO}/include/dfr/recovery/client.hpp`,
  },
  {
    title: "Six million hostile inputs, and the check that made the result mean anything",
    hid: "Every decoder takes bytes off a network, and the unit tests feed them bytes a person chose — which finds the cases a person thought of. Six targets, sanitizers on, seeded with real IEX packets so every mutation starts from something that parses.",
    caught:
      "Nothing, across 6,000,000 mutations. Which is worth nothing unless the fuzzer can find things, so the length check in the DEEP header decoder was deliberately removed to see whether it would.",
    matters:
      "It did — by the self-consistency check, not by AddressSanitizer. The sanitizer only reports the over-read when the short message happens to sit at the end of an allocation; \u201ca decode reported a length it did not have\u201d catches it every time.",
    kind: "hostile input",
    where: `${REPO}/docs/FUZZING.md`,
  },
  {
    title: "A benchmark loop the optimiser deleted",
    hid: "Polling a synchronised client reported 0.0 ns and ninety-seven billion polls a second. The client's state never changed, so the loop was invariant and -O3 computed one answer and kept it.",
    caught: "The number being too good. A barrier on the loop's result does not help — the fix is a real data dependency, so each iteration's input comes from the last one's output.",
    matters: "Two more measurements were wrong the same way: one timed a 53 KB constructor and called it “one poll”, another divided a nine-operation setup by one. Every measurement now names its unit in the output.",
    kind: "benchmarking",
    where: `${REPO}/docs/BENCHMARKS.md`,
  },
];

/**
 * How many tests there are, and why the number is written here rather than passed in.
 *
 * It is generated by `scripts/sync-test-count.sh` from `ctest -N`, which is the only honest way to have it on a
 * static page: hardcoding it means it silently becomes a lie the next time a suite is added, and the viewer
 * cannot run ctest. The script fails the build if it drifts, so the page and the build agree or neither ships.
 */
export const TEST_COUNT = 720;

/** What a visitor with thirty seconds should be able to read without scrolling or pressing anything. */
export interface Evidence {
  readonly value: string;
  readonly label: string;
  readonly title: string;
}

export function evidence(tests: number, allocations: number, perPacket: string): readonly Evidence[] {
  return [
    {
      value: String(tests),
      label: "tests",
      title: "under five configurations: assertions paranoid, fast and off, plus AddressSanitizer and ThreadSanitizer",
    },
    {
      value: "3",
      label: "compilers, all with warnings as errors",
      title:
        "Apple Clang locally, Linux Clang and GCC 14 in CI. Each has found defects the others never mention — " +
        "and the direction is not fixed: GCC caught three false constexpr claims, and the local arm64 machine is " +
        "the only place a broken memory ordering is visible at all.",
    },
    { value: String(allocations), label: "allocations after start-up", title: "counted by replacing the global operator new across a whole recovery run" },
    { value: perPacket, label: "to take in one packet", title: "one core, no I/O in the loop, measured natively" },
  ];
}

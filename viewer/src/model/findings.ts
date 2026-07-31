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
// Every entry below is a real commit. The claim, the way it hid, and the thing that caught it. Four of the six
// are mistakes I made and then found; that is the point rather than an embarrassment, because a portfolio of
// things that went right is a portfolio of things nobody checked.

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
export const TEST_COUNT = 685;

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
      title: "run under five configurations: assertions paranoid, fast and off, plus AddressSanitizer and ThreadSanitizer",
    },
    { value: "5", label: "sanitiser and assertion builds", title: "dev, release, bench, asan, tsan — all with warnings as errors" },
    { value: String(allocations), label: "allocations after start-up", title: "counted by replacing the global operator new across a whole recovery run" },
    { value: perPacket, label: "to take in one packet", title: "one core, no I/O in the loop, measured natively" },
  ];
}

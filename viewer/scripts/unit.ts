// Unit tests for the model layer's pure functions, exercised directly rather than through a render.
//
// scripts/check.tsx (via scripts/checks/*) measures the *drawing*: it renders panels against the three
// committed traces and asserts on the resulting markup. That leaves a real gap: plainly(), verdictAt() and
// lineTallies() are each called with whatever those three specific traces happen to contain, which is not the
// same as calling them with the edge cases their own logic branches on: an unsettled run, a snapshot-rejected
// run, a tie between two equally fast measurements. This file builds those cases by hand instead of hoping a
// committed trace happens to contain one.
//
// Also where a dead-code finding lives: five of select.ts's six exported functions had no caller anywhere in
// the app, written in the viewer's first commit and never used since. Asking "what would I even test here"
// is what surfaced it, and it is deleted rather than covered, since a test for a function nothing calls asserts nothing
// about the app.

import { plainly } from "../src/model/plain";
import { verdictAt } from "../src/model/book";
import { lineTallies } from "../src/model/select";
import { compact, fastest, ratePerSecond } from "../src/model/here";
import { check, report } from "./reporter";
import { event, trace } from "./fixtures";

// ---------------------------------------------------------------------------
// plainly()
// ---------------------------------------------------------------------------

console.log("plainly(): the run in plain language");

{
  const t = trace([], { messages: 100 });
  const unsettled = plainly(t, undefined);
  check(!unsettled.settled, "with no event yet, the run has not settled");
  check(/still going/.test(unsettled.result), "and the result says so rather than guessing");
  check(/Watch what/.test(unsettled.response), "and the response defers rather than reporting a repair");
}

{
  // delivered_before short of messages, and not failed: still running, even with an event to read.
  const t = trace([], { messages: 100 });
  const partway = plainly(t, event({ delivered_before: 40, state: "recovering" }));
  check(!partway.settled, "short of the message count and not failed, the run is not settled");
}

{
  // A failed client stalls short of the message count on purpose, and that must still count as settled: it
  // is never going to deliver anything else, and that is exactly the act whose outcome is worth stating. A
  // client fails because a snapshot was needed and behind_buffer, which is where the unfillable count comes
  // from, so the fixture states one rather than leaving it at the default zero.
  const t = trace([], { messages: 100 }, { messages_missing: 20, unfillable_messages: 20 });
  const failed = plainly(t, event({ delivered_before: 40, state: "failed" }));
  check(failed.settled, "a failed client settles even though delivery stalled short");
  check(!failed.good, "and it does not read as a good outcome");
}

{
  const t = trace([], { messages: 100 }, { messages_missing: 0, unfillable_messages: 0 });
  const clean = plainly(t, event({ delivered_before: 100 }));
  check(clean.settled && clean.good, "nothing missing settles as a good outcome");
  check(/None of them/.test(clean.lost), "and says nothing went missing, in those words");
  check(/Nothing went missing/.test(clean.response), "and the response matches: nothing to repair");
}

{
  // Singular vs. plural: "1 time" reads as English, "1 times" reads as a template that forgot to check.
  const once = trace(
    [],
    { messages: 100 },
    { messages_missing: 5, unfillable_messages: 0, retransmit_requests: 1 },
  );
  const said = plainly(once, event({ delivered_before: 100 }));
  check(/asked the exchange to send them again, 1 time\./.test(said.response), "one request reads as \"1 time\"");

  const thrice = trace(
    [],
    { messages: 100 },
    { messages_missing: 5, unfillable_messages: 0, retransmit_requests: 3 },
  );
  const saidPlural = plainly(thrice, event({ delivered_before: 100 }));
  check(/3 times\./.test(saidPlural.response), "three requests read as \"3 times\"");
}

{
  // A snapshot changes what the response says it did, not just how many times.
  const t = trace(
    [],
    { messages: 100 },
    { messages_missing: 20, unfillable_messages: 0, retransmit_requests: 2, snapshot_requests: 1 },
  );
  const said = plainly(t, event({ delivered_before: 100 }));
  check(/fresh copy of the whole price list/.test(said.response), "a snapshot run says so, distinctly from a retransmit-only run");
}

{
  // Lost for good: the outcome that must not read as a success.
  const t = trace([], { messages: 100 }, { messages_missing: 30, unfillable_messages: 12 });
  const said = plainly(t, event({ delivered_before: 100 }));
  check(!said.good, "data lost for good never reads as a good outcome");
  check(/never came back/.test(said.result), "and says so, rather than only reporting the good acts silently");
  check(/12/.test(said.lost), "the lost-for-good count is stated, not folded into \"missing\"");
}

console.log("\nverdictAt(): the book this act rebuilt, against the one that lost nothing");

{
  const t = trace([], {}, { reference_bid: 1000, reference_ask: 1010, reference_traded: 500 });
  const nothingYet = verdictAt(t, undefined);
  check(!nothingYet.comparable, "before any event, there is nothing to compare yet");
  check(nothingYet.missingShares === 0, "and no shares are reported missing for a comparison that has not happened");
}

{
  const t = trace([], { messages: 100 }, { reference_bid: 1000, reference_ask: 1010, reference_traded: 500 });
  const midRun = verdictAt(t, event({ delivered_before: 40, state: "recovering" }));
  check(!midRun.comparable, "mid-run, the reference book and this book are not yet comparable");
}

{
  const t = trace([], { messages: 100 }, { reference_bid: 1000, reference_ask: 1010, reference_traded: 500 });
  const matching = verdictAt(
    t,
    event({ delivered_before: 100, best_bid: 1000, best_ask: 1010, traded_shares: 500 }),
  );
  check(matching.comparable && matching.matches, "a finished run with the same book matches, and is comparable");
  check(matching.missingShares === 0, "and reports no missing shares");
}

{
  // A failed client's book is comparable (the run is over) and, having lost data, does not match.
  const t = trace([], { messages: 100 }, { reference_bid: 1000, reference_ask: 1010, reference_traded: 500 });
  const broken = verdictAt(
    t,
    event({ delivered_before: 40, state: "failed", best_bid: 1000, best_ask: 1010, traded_shares: 231 }),
  );
  check(broken.comparable, "a failed client's outcome is comparable even though delivery stalled");
  check(!broken.matches, "and the mismatched trade count means it does not match");
  check(broken.missingShares === 500 - 231, "missing shares is the reference less what this run actually traded");
}

{
  // Never negative: a run cannot have traded *more* than the loss-free reference without something else being
  // wrong, but the arithmetic must not report a negative number of missing shares if it ever did.
  const t = trace([], { messages: 100 }, { reference_traded: 500 });
  const over = verdictAt(t, event({ delivered_before: 100, traded_shares: 600 }));
  check(over.missingShares === 0, "missing shares never goes negative");
}

console.log("\nlineTallies(): what each line of a redundant pair actually carried");

{
  const empty = lineTallies(trace([]));
  check(empty.length === 0, "no events, no lines");
}

{
  const t = trace([
    event({ line: 0, event: "packet_accepted" }),
    event({ line: 0, event: "packet_duplicate" }),
    event({ line: 0, event: "packet_discarded" }),
    event({ line: 0, event: "packet_dropped" }),
    event({ line: 1, event: "gap_filled" }),
  ]);
  const tallies = lineTallies(t);
  check(tallies.length === 2, "one entry per line that appeared, not per line configured");
  const line0 = tallies.find((l) => l.line === 0)!;
  check(line0.won === 1 && line0.duplicate === 1 && line0.discarded === 1 && line0.faults === 1, "line 0's four counts each land in the right bucket");
  const line1 = tallies.find((l) => l.line === 1)!;
  check(line1.won === 1, "gap_filled counts as won, the same as a plain accept");
}

{
  const t = trace([event({ line: 5, event: "packet_accepted" }), event({ line: 1, event: "packet_accepted" })]);
  const tallies = lineTallies(t);
  check(
    tallies[0]!.line === 1 && tallies[1]!.line === 5,
    "lines are sorted ascending, not in the order they first appeared",
  );
}

{
  // An event kind this file does not recognise must not throw, and must not be guessed into one of the four
  // buckets. What it actually does: the line gets no entry at all, since nothing about it was worth counting.
  // Verified the true behaviour before asserting it, rather than assuming a zeroed entry and having the test
  // itself crash on a wrong guess (which is exactly what the first version of this test did).
  const t = trace([event({ line: 0, event: "something_new" })]);
  const tallies = lineTallies(t);
  check(
    tallies.find((l) => l.line === 0) === undefined,
    "a line with only an unrecognised event kind gets no entry, rather than a guessed one",
  );
}

console.log("\nhere.ts: the one performance figure a reader causes");

{
  check(ratePerSecond(undefined) === undefined, "no run yet is not a rate of zero");
  check(ratePerSecond({ elapsedMs: 0, messages: 100, runs: 1 }) === undefined, "zero elapsed time is not a rate, not a division by zero");
  check(ratePerSecond({ elapsedMs: 10, messages: 0, runs: 1 }) === undefined, "zero messages is not a rate either");

  const real = ratePerSecond({ elapsedMs: 10, messages: 1000, runs: 1 });
  check(real !== undefined && real.messagesPerSecond === 100_000, "10ms for 1,000 messages is 100,000 a second");
  check(real !== undefined && real.microsPerMessage === 10, "and 10 microseconds each");
}

{
  // A tie keeps the earlier run rather than replacing it: `fastest` compares strictly, so equally fast
  // measurements do not flap between which one is reported.
  const first = fastest(undefined, { elapsedMs: 10, messages: 1000, runs: 1 });
  const tie = fastest(first, { elapsedMs: 5, messages: 500, runs: 1 }); // same per-message rate, 10us either way
  check(tie.elapsedMs === first.elapsedMs && tie.messages === first.messages, "an equally fast run does not replace the one already held");
  check(tie.runs === 2, "but it is still counted");
}

check(compact(930) === "930", "a small count is printed exactly");
check(compact(48_400) === "48K", "thousands round to the nearest whole K");
check(compact(1_234_567) === "1.2M", "millions keep one decimal place");

console.log(report.failures === 0 ? "\nall unit checks passed" : `\n${report.failures} unit checks failed`);
process.exit(report.failures === 0 ? 0 : 1);

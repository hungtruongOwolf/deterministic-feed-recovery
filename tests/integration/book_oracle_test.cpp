// The invariant this project was always trying to reach.
//
// Everything before this proved a statement about *bookkeeping*: every sequence number arrived exactly once, no
// gap went unnoticed, nothing was delivered twice. All true, all necessary, and none of it is what a trading
// system needs to hear. What it needs to hear is a statement about *content*:
//
//   **the book after loss and repair is the book that would have existed if nothing had been lost.**
//
// That was not expressible until the messages meant something. It is expressible now, and it is a much harder
// invariant than the sequence one: it fails if recovery delivers the right messages in the wrong order, or
// applies a repair twice, or drops a size-zero deletion, none of which a sequence count can see.
//
// How the test works
// ------------------
// One stream of DEEP messages is published once. It is then consumed twice:
//
//   * the reference: every packet handed straight to a book, in order, nothing lost;
//   * the subject: the same packets through the fault injector and recovery::client, which loses some, notices,
//     asks for them back, and replays what it recovered.
//
// The two books must be equal at the end. The reference is not a second implementation of anything: it is the
// same decoder and the same book, fed the packets the venue sent, which is what makes the comparison mean
// "recovery lost nothing" rather than "two implementations agree".
//
// What writing this found
// ----------------------
// The first version applied messages in the order the client delivered them, and the books did not match: same
// 600 messages, same update counts, different book. Recovery was right and the consumer was wrong: see the last
// test in this file, which keeps that mistake alive on purpose.

#include "support/oracle_replay.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace dfr_test::oracle;  // NOLINT(google-build-using-namespace)

TEST_CASE("with no loss at all, the two books agree trivially") {
  // The control. If this ever fails, the harness is broken and every result below is meaningless, so it is
  // asserted rather than assumed, before anything interesting is tried.
  const auto feed = publish_feed(600, /*symbols=*/4);
  const auto reference = replay_clean(feed);
  const auto through = replay_recovered(feed, /*seed=*/1, /*faults=*/0);

  CHECK(through.delivered == reference.delivered);
  CHECK(through.books == reference.books);
  CHECK(through.unfillable == 0);
}

TEST_CASE("the book survives loss and repair, and equals the book that lost nothing") {
  const auto feed = publish_feed(600, 4);
  const auto reference = replay_clean(feed);

  // Several patterns, because one is an anecdote. Each damages a different set of packets.
  std::uint64_t patterns_with_a_gap = 0;
  for (const std::uint64_t seed : {1ULL, 7ULL, 4711ULL, 90210ULL, 555ULL, 31337ULL}) {
    const auto through = replay_recovered(feed, seed, /*faults=*/8);

    CHECK(through.unfillable == 0);
    CHECK(through.delivered == reference.delivered);
    // The assertion the whole file exists for.
    CHECK(through.books == reference.books);
    patterns_with_a_gap += through.gaps_opened > 0 ? 1 : 0;
  }

  // The losses have to be real somewhere, or every equality above is a run where nothing happened. Counted
  // across the patterns rather than asserted per pattern: at seed 7 the injector's draws happen to damage only
  // packets that reorder without opening a hole, which is a legitimate run and not a reason to fail.
  CHECK(patterns_with_a_gap >= 4);
}

TEST_CASE("a repair applied twice would be caught, and is not applied twice") {
  const auto feed = publish_feed(400, 3);
  const auto reference = replay_clean(feed);
  // Duplication is the fault a sequence count is blindest to: delivering a message twice keeps every sequence
  // accounted for and doubles a price level's size. An aggregated book *replaces* rather than adds, so a
  // duplicate price level is invisible here, but a duplicated *trade* is not, and the counts catch it.
  const auto through = replay_recovered(feed, /*seed=*/31337, /*faults=*/10);

  CHECK(through.books == reference.books);
  CHECK(through.traded_shares == reference.traded_shares);
  CHECK(through.trades == reference.trades);
}

TEST_CASE("when a snapshot answers too late, the book says it is incomplete rather than wrong") {
  const auto feed = publish_feed(600, 4);
  const auto reference = replay_clean(feed);
  // The glimpse race: retransmits are refused, so the client must fall back to a snapshot, and the snapshot is
  // older than the oldest message the client managed to buffer. Messages exist in neither.
  const auto through = replay_recovered(feed, /*seed=*/4711, /*faults=*/8, /*glimpse=*/true);

  CHECK(through.unfillable > 0);
  // The book is *not* equal, and that is the correct outcome. What matters is that the client knows: a client
  // that carried on here would publish a book that looks complete and is permanently wrong.
  CHECK_FALSE(through.books == reference.books);
  CHECK(through.failed);
}


TEST_CASE("applying a gap-filling feed in arrival order gives the wrong book") {
  const auto feed = publish_feed(600, 4);
  const auto reference = replay_clean(feed);

  // The trap, kept because a hazard nobody demonstrates is a hazard everybody rediscovers.
  //
  // While a hole is open the client keeps delivering later messages: deliberately, because stalling on a gap
  // turns one loss into an outage. So a repair arrives *after* messages with higher sequence numbers, and an
  // aggregated book is last-write-wins: applying the older update second leaves the wrong size at that price,
  // permanently.
  //
  // Everything else about the two runs is identical. Same messages, same count, same trades.
  const auto ordered = replay_recovered(feed, /*seed=*/1, /*faults=*/8);
  const auto naive =
      replay_recovered(feed, /*seed=*/1, /*faults=*/8, /*glimpse=*/false, /*apply_in_arrival_order=*/true);

  CHECK(naive.delivered == ordered.delivered);
  CHECK(naive.trades == ordered.trades);
  CHECK(ordered.books == reference.books);
  // The consumer that did not sort is wrong, and nothing told it so. That is the point.
  CHECK_FALSE(naive.books == reference.books);
}

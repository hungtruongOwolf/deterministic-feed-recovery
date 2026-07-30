// The tracker's sequencing outcomes: in order, gap opened, gap filled, duplicate.
//
// The distinction being tested throughout is remembering versus detecting. A checker
// reports an inconsistency and forgets it; the tracker has to still know, twenty
// packets later, that sequences 40 through 44 never arrived, and has to recognise the
// retransmit when it comes.
//
// The events that throw state away — session changes and snapshots — are in
// gap_tracker_reset_test.cpp, because they are about forgetting rather than tracking.

#include <dfr/recovery/gap_tracker.hpp>

#include "recovery/support/tracker_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace rec = dfr::recovery;

using dfr_test::recovery::feed;
using dfr_test::recovery::kChannel;
using dfr_test::recovery::missing;
using dfr_test::recovery::range;

// ---------------------------------------------------------------------------
// The ordinary path
// ---------------------------------------------------------------------------

TEST_CASE("the first packet establishes the channel rather than reporting a gap",
          "[recovery][gap_tracker]") {
  // A receiver joining a live feed mid-session has missed everything before it, and
  // must not report that as a hole it could recover — the publisher's retention
  // window does not go back to the start of the day.
  rec::gap_tracker tracker;
  CHECK_FALSE(tracker.started(kChannel));

  const auto first = feed(tracker, 1'000, 5);
  CHECK(first.outcome == rec::sequencing::established);
  CHECK(first.code() == dfr::error::ok);
  CHECK(tracker.started(kChannel));
  CHECK(tracker.expected_sequence(kChannel) == 1'005);
  CHECK(tracker.total_missing() == 0);
}

TEST_CASE("a contiguous stream stays in order and misses nothing",
          "[recovery][gap_tracker]") {
  rec::gap_tracker tracker;
  std::uint64_t sequence = 1;
  for (int i = 0; i < 50; ++i) {
    const auto seen = feed(tracker, sequence, 3);
    CHECK(seen.outcome ==
          (i == 0 ? rec::sequencing::established : rec::sequencing::in_order));
    sequence += 3;
  }
  CHECK(tracker.total_missing() == 0);
  CHECK(tracker.stats(kChannel).packets == 50);
  CHECK(tracker.stats(kChannel).messages == 150);
  CHECK(tracker.stats(kChannel).gaps_opened == 0);
}

TEST_CASE("a heartbeat is not a special case", "[recovery][gap_tracker]") {
  // Count 0, carrying the sequence number of the next message. It must not advance
  // the expectation past itself, which falls out of first + 0 == first rather than
  // needing a branch — and it must still be able to open a gap, because a heartbeat
  // whose sequence number has jumped is exactly how a receiver learns it missed the
  // end of a quiet period.
  rec::gap_tracker tracker;
  feed(tracker, 1, 4);
  REQUIRE(tracker.expected_sequence(kChannel) == 5);

  const auto beat = feed(tracker, 5, 0);
  CHECK(beat.outcome == rec::sequencing::in_order);
  CHECK(tracker.expected_sequence(kChannel) == 5);
  CHECK(tracker.total_missing() == 0);

  const auto jumped = feed(tracker, 9, 0);
  CHECK(jumped.outcome == rec::sequencing::gap_opened);
  CHECK(jumped.range == range(5, 9));
  CHECK(tracker.total_missing() == 4);
}

// ---------------------------------------------------------------------------
// Opening a gap
// ---------------------------------------------------------------------------

TEST_CASE("a jump forward opens a gap and names the missing range",
          "[recovery][gap_tracker]") {
  // Naming the range is the point. A caller that only learns "something was missed"
  // cannot build a retransmit request.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);  // 1..10, expect 11

  const auto jumped = feed(tracker, 20, 5);
  CHECK(jumped.outcome == rec::sequencing::gap_opened);
  CHECK(jumped.range == range(11, 20));
  CHECK(jumped.code() == dfr::error::sequence_gap);
  CHECK_FALSE(dfr::is_fatal(jumped.code()));  // recoverable, and the normal case

  CHECK(missing(tracker) == std::vector{range(11, 20)});
  CHECK(tracker.total_missing() == 9);
  CHECK(tracker.stats(kChannel).messages_missed == 9);
}

TEST_CASE("live data past a gap keeps flowing", "[recovery][gap_tracker]") {
  // The behaviour that separates a tracker from a checker. A receiver must not stall
  // on a hole: it keeps delivering what arrives while recovery runs in parallel.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);  // gap 11..19

  for (std::uint64_t seq = 25; seq < 100; seq += 5) {
    CHECK(feed(tracker, seq, 5).outcome == rec::sequencing::in_order);
  }
  CHECK(missing(tracker) == std::vector{range(11, 20)});
  CHECK(tracker.stats(kChannel).gaps_opened == 1);
}

TEST_CASE("separate bursts of loss are separate holes",
          "[recovery][gap_tracker]") {
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);  // 11..19 missing
  feed(tracker, 40, 5);  // 25..39 missing

  CHECK(missing(tracker) == std::vector{range(11, 20), range(25, 40)});
  CHECK(tracker.total_missing() == 9 + 15);
  CHECK(tracker.stats(kChannel).gaps_opened == 2);
}

// ---------------------------------------------------------------------------
// Filling a gap
// ---------------------------------------------------------------------------

TEST_CASE("a retransmit that covers the hole closes it",
          "[recovery][gap_tracker]") {
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);
  REQUIRE(tracker.total_missing() == 9);

  const auto repair = feed(tracker, 11, 9);
  CHECK(repair.outcome == rec::sequencing::gap_filled);
  CHECK(repair.recovered == 9);
  CHECK(repair.code() == dfr::error::ok);  // good news, not an error
  CHECK(tracker.total_missing() == 0);
  CHECK(tracker.stats(kChannel).messages_recovered == 9);
}

TEST_CASE("a retransmit does not move the expectation backwards",
          "[recovery][gap_tracker]") {
  // The mistake that turns one hole into an endless stream of them: rewinding the
  // expectation to the retransmit's position would make every live packet already
  // received look like a fresh gap.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);  // expect 25
  REQUIRE(tracker.expected_sequence(kChannel) == 25);

  feed(tracker, 11, 9);
  CHECK(tracker.expected_sequence(kChannel) == 25);
}

TEST_CASE("a partial retransmit leaves the rest outstanding",
          "[recovery][gap_tracker]") {
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);  // 11..19 missing

  const auto partial = feed(tracker, 11, 4);  // 11..14 only
  CHECK(partial.outcome == rec::sequencing::gap_filled);
  CHECK(partial.recovered == 4);
  CHECK(missing(tracker) == std::vector{range(15, 20)});
}

TEST_CASE("a retransmit landing inside a hole splits it",
          "[recovery][gap_tracker]") {
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 30, 5);  // 11..29 missing

  const auto middle = feed(tracker, 18, 4);  // 18..21
  CHECK(middle.outcome == rec::sequencing::gap_filled);
  CHECK(middle.recovered == 4);
  CHECK(missing(tracker) == std::vector{range(11, 18), range(22, 30)});
}

TEST_CASE("a packet straddling the expectation both fills and extends",
          "[recovery][gap_tracker]") {
  // A retransmit that also carries fresh messages. The tail beyond the expectation
  // is new and contiguous, so the expectation advances and no new gap opens — a
  // tracker that only handled "behind" or "ahead" would either drop the tail or
  // report a gap that does not exist.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);  // 11..19 missing, expect 25

  const auto straddle = feed(tracker, 15, 15);  // 15..29
  CHECK(straddle.outcome == rec::sequencing::gap_filled);
  CHECK(straddle.recovered == 5);  // only 15..19 were missing
  CHECK(missing(tracker) == std::vector{range(11, 15)});
  CHECK(tracker.expected_sequence(kChannel) == 30);
}

TEST_CASE("a duplicate is distinguished from a retransmit",
          "[recovery][gap_tracker]") {
  // Both arrive behind the expectation. Only the gap set can tell them apart, which
  // is why the sequence number alone does not decide.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 11, 5);  // in order, expect 16

  const auto again = feed(tracker, 11, 5);
  CHECK(again.outcome == rec::sequencing::duplicate);
  CHECK(again.recovered == 0);
  CHECK(again.code() == dfr::error::sequence_regressed);
  CHECK(tracker.stats(kChannel).duplicates == 1);
  CHECK(tracker.total_missing() == 0);
}

TEST_CASE("the late half of an A/B pair is a duplicate, not a gap",
          "[recovery][gap_tracker]") {
  // Feeding both lines of a redundant pair into one tracker is the normal case for
  // arbitration, and every packet arrives twice. If the second copy were treated as
  // anything but routine, an operator would see the duplicate count as the error rate.
  rec::gap_tracker tracker;
  std::uint64_t sequence = 1;
  for (int i = 0; i < 20; ++i) {
    feed(tracker, sequence, 5);  // line A
    feed(tracker, sequence, 5);  // line B, arriving second
    sequence += 5;
  }
  CHECK(tracker.total_missing() == 0);
  // Twenty, not nineteen: the first packet of the first pair establishes the channel,
  // but the *second* copy of it is already a duplicate like all the rest.
  CHECK(tracker.stats(kChannel).duplicates == 20);
  CHECK(tracker.stats(kChannel).gaps_opened == 0);
}

TEST_CASE("a gap on one line is filled by the other",
          "[recovery][gap_tracker]") {
  // A/B arbitration in its simplest form: line A drops a burst, line B carries it,
  // and the receiver loses nothing. This is the property the whole redundant-feed
  // arrangement exists to provide.
  rec::gap_tracker tracker;
  feed(tracker, 1, 5);    // A
  feed(tracker, 11, 5);   // A jumped: 6..10 missing
  REQUIRE(tracker.total_missing() == 5);

  const auto from_b = feed(tracker, 6, 5);  // B still had them
  CHECK(from_b.outcome == rec::sequencing::gap_filled);
  CHECK(from_b.recovered == 5);
  CHECK(tracker.total_missing() == 0);
}


// The events that throw tracker state away, and the limits on how much it holds.
//
// Session changes and snapshots are grouped together because they are the same kind
// of event: something has made the sequence numbers the receiver holds stop meaning
// what it thought. Both discard holes, and both have to report how much was
// abandoned, because giving up on a hole is data loss even when it is correct.

#include <dfr/recovery/gap_tracker.hpp>

#include "recovery/support/tracker_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rec = dfr::recovery;

using dfr_test::recovery::feed;
using dfr_test::recovery::kChannel;
using dfr_test::recovery::kSession;
using dfr_test::recovery::missing;
using dfr_test::recovery::range;

// ---------------------------------------------------------------------------
// Session change and snapshots
// ---------------------------------------------------------------------------

TEST_CASE("a session change discards the holes and counts them as lost",
          "[recovery][gap_tracker]") {
  // Outstanding holes belong to the old session. Requesting them from the new one
  // would be asking for sequence numbers that mean something else there — and the
  // publisher would answer.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);
  REQUIRE(tracker.total_missing() == 9);

  const auto reset = feed(tracker, 1, 5, 0x9999);
  CHECK(reset.outcome == rec::sequencing::session_reset);
  CHECK(reset.code() == dfr::error::session_changed);
  CHECK(dfr::is_fatal(reset.code()));

  CHECK(tracker.total_missing() == 0);
  CHECK(tracker.expected_sequence(kChannel) == 6);
  CHECK(tracker.stats(kChannel).messages_abandoned == 9);
  CHECK(tracker.stats(kChannel).session_resets == 1);
}

TEST_CASE("a snapshot abandons the holes below it and reports the loss",
          "[recovery][gap_tracker]") {
  // The accounting the Glimpse race destroys when it goes unnoticed. A snapshot
  // behind the buffer produces a plausible book permanently missing messages that
  // nobody counted; here they are counted.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);   // 11..19 missing
  feed(tracker, 40, 5);   // 25..39 missing
  REQUIRE(tracker.total_missing() == 9 + 15);

  CHECK(tracker.snapshot_at(kChannel, kSession, 30) == 9 + 5);
  CHECK(missing(tracker) == std::vector{range(30, 40)});
  CHECK(tracker.stats(kChannel).messages_abandoned == 14);
}

TEST_CASE("a snapshot ahead of the stream moves the expectation forward",
          "[recovery][gap_tracker]") {
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  REQUIRE(tracker.expected_sequence(kChannel) == 11);

  CHECK(tracker.snapshot_at(kChannel, kSession, 500) == 0);
  CHECK(tracker.expected_sequence(kChannel) == 500);
  CHECK(feed(tracker, 500, 5).outcome == rec::sequencing::in_order);
}

TEST_CASE("a snapshot behind the stream does not rewind it",
          "[recovery][gap_tracker]") {
  // A stale snapshot must not undo progress already made from live data, or every
  // packet since would be replayed as a gap.
  rec::gap_tracker tracker;
  feed(tracker, 1, 100);
  REQUIRE(tracker.expected_sequence(kChannel) == 101);

  CHECK(tracker.snapshot_at(kChannel, kSession, 50) == 0);
  CHECK(tracker.expected_sequence(kChannel) == 101);
}

TEST_CASE("a snapshot can start a channel that has seen no packets",
          "[recovery][gap_tracker][regression]") {
  // Recovering before joining the live feed: the snapshot is the first thing the
  // receiver has, and the first live packet after it must chain rather than
  // establish.
  //
  // This is why snapshot_at() takes a session. Without one it could not record which
  // session it had established state for, so the first live packet compared against a
  // default of zero and came back session_reset — fatal — and the receiver would have
  // discarded the snapshot it had just successfully applied.
  rec::gap_tracker tracker;
  CHECK(tracker.snapshot_at(kChannel, kSession, 700) == 0);
  CHECK(tracker.started(kChannel));
  CHECK(feed(tracker, 700, 3).outcome == rec::sequencing::in_order);
  CHECK(tracker.expected_sequence(kChannel) == 703);
}

TEST_CASE("a snapshot of a different session supersedes everything held",
          "[recovery][gap_tracker]") {
  // The sequence numbers in the outstanding holes mean something else in the new
  // session, so carrying them forward would make the receiver request ranges that
  // exist there and get plausible, wrong answers.
  rec::gap_tracker tracker;
  feed(tracker, 1, 10);
  feed(tracker, 20, 5);  // 11..19 missing
  REQUIRE(tracker.total_missing() == 9);

  CHECK(tracker.snapshot_at(kChannel, 0x9999, 4) == 9);
  CHECK(tracker.total_missing() == 0);
  // Even though 4 is behind the old session's position: it is not the same stream, so
  // there is no progress to protect.
  CHECK(tracker.expected_sequence(kChannel) == 4);
  CHECK(feed(tracker, 4, 2, 0x9999).outcome == rec::sequencing::in_order);
}

// ---------------------------------------------------------------------------
// Channels are independent
// ---------------------------------------------------------------------------

TEST_CASE("channels do not share sequence state", "[recovery][gap_tracker]") {
  // The bug the channel_id type exists to prevent, stated as a property: one
  // channel's numbering must never be measured against another's.
  rec::gap_tracker tracker;
  const auto other = rec::channel_id::at(1);
  rec::observation seen;

  REQUIRE(tracker.observe(kChannel, kSession, 1, 10).get(seen) ==
          dfr::error::ok);
  REQUIRE(tracker.observe(other, kSession, 5'000, 10).get(seen) ==
          dfr::error::ok);
  CHECK(seen.outcome == rec::sequencing::established);  // not a gap of 4,990

  REQUIRE(tracker.observe(kChannel, kSession, 11, 5).get(seen) ==
          dfr::error::ok);
  CHECK(seen.outcome == rec::sequencing::in_order);
  CHECK(tracker.total_missing() == 0);
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

TEST_CASE("too many holes is reported, and the expectation does not advance",
          "[recovery][gap_tracker][regression]") {
  // Advancing past a packet whose hole could not be recorded would mean the tracker
  // accepted the packet and forgot the hole in the same step — the next packet would
  // then look in-order while a whole range had vanished from the accounting. Refusing
  // and standing still keeps the picture honest.
  rec::gap_tracker tracker;
  feed(tracker, 1, 1);
  std::uint64_t sequence = 2;
  for (std::size_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    sequence += 10;  // skip 9, leaving a hole
    feed(tracker, sequence, 1);
    sequence += 1;
  }
  REQUIRE(tracker.outstanding(kChannel).size() == rec::kMaxOutstandingGaps);

  const auto expected_before = tracker.expected_sequence(kChannel);
  const auto refused = tracker.observe(kChannel, kSession, sequence + 100, 1);
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
  CHECK(tracker.expected_sequence(kChannel) == expected_before);
}

TEST_CASE("every sequencing outcome has a distinct name",
          "[recovery][gap_tracker]") {
  // So a report or a log line cannot silently print the wrong one, and so adding an
  // outcome without naming it fails to compile rather than printing an empty string.
  std::vector<std::string_view> names;
  for (std::size_t i = 0; i < rec::kSequencingCount; ++i) {
    names.push_back(rec::name_of(static_cast<rec::sequencing>(i)));
  }
  for (const auto& name : names) {
    CHECK_FALSE(name.empty());
  }
  std::vector<std::string_view> sorted = names;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
}

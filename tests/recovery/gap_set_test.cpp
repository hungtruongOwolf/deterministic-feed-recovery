// The outstanding-gap set: canonical form, and what happens at capacity.
//
// Every test here is really one of two questions. Does the set still describe
// exactly what is missing: no more, no less? And when it cannot, does it say so
// rather than rounding?

#include <dfr/recovery/gap_set.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace rec = dfr::recovery;

namespace {

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

std::vector<rec::sequence_range> listed(const rec::gap_set& set) {
  return {set.ranges().begin(), set.ranges().end()};
}

// Opens a hole and requires it to be accepted, for the setup steps whose failure
// would be a bug in the test rather than the point of it.
void must_open(rec::gap_set& set, rec::sequence_range hole) {
  REQUIRE(set.open(hole).has_value());
}

}  // namespace

// ---------------------------------------------------------------------------
// Canonical form
// ---------------------------------------------------------------------------

TEST_CASE("a fresh set is missing nothing", "[recovery][gap_set]") {
  const rec::gap_set set;
  CHECK(set.empty());
  CHECK(set.size() == 0);
  CHECK(set.total_missing() == 0);
}

TEST_CASE("holes are kept sorted regardless of the order they arrive",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(50, 60));
  must_open(set, range(10, 20));
  must_open(set, range(30, 40));

  CHECK(listed(set) == std::vector{range(10, 20), range(30, 40), range(50, 60)});
  CHECK(set.oldest() == range(10, 20));
  CHECK(set.total_missing() == 30);
}

TEST_CASE("adjacent holes become one request", "[recovery][gap_set]") {
  // The property the whole class exists for. Losing 10..19 and then 20..29 is one
  // burst and must produce one retransmit request, not two.
  rec::gap_set set;
  must_open(set, range(10, 20));
  must_open(set, range(20, 30));

  CHECK(set.size() == 1);
  CHECK(set.oldest() == range(10, 30));
  CHECK(set.total_missing() == 20);
}

TEST_CASE("overlapping holes are counted once", "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 25));
  must_open(set, range(20, 30));

  CHECK(set.size() == 1);
  CHECK(set.oldest() == range(10, 30));
  CHECK(set.total_missing() == 20);  // not 15 + 10
}

TEST_CASE("a hole can bridge two existing ones", "[recovery][gap_set]") {
  // The reason open() scans the whole array instead of stopping at the first range
  // it does not touch. 10..19 and 30..39 are not adjacent to each other, but 20..29
  // joins them into a single range; stopping early would leave two ranges that are
  // now adjacent, which is outside canonical form and would produce two requests
  // for one contiguous hole.
  rec::gap_set set;
  must_open(set, range(10, 20));
  must_open(set, range(30, 40));
  REQUIRE(set.size() == 2);

  must_open(set, range(20, 30));
  CHECK(set.size() == 1);
  CHECK(set.oldest() == range(10, 40));
  CHECK(set.total_missing() == 30);
}

TEST_CASE("a hole entirely inside a known one changes nothing",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 40));
  must_open(set, range(20, 25));

  CHECK(set.size() == 1);
  CHECK(set.oldest() == range(10, 40));
  CHECK(set.total_missing() == 30);
}

TEST_CASE("opening an empty hole is accepted and does nothing",
          "[recovery][gap_set]") {
  // So a caller computing a range arithmetically does not have to check first.
  rec::gap_set set;
  must_open(set, range(7, 7));
  CHECK(set.empty());
}

TEST_CASE("asking for the oldest gap when nothing is missing aborts",
          "[recovery][gap_set]") {
  DFR_CHECK_ABORTS({
    const rec::gap_set set;
    (void)set.oldest();
  });
}

// ---------------------------------------------------------------------------
// Filling
// ---------------------------------------------------------------------------

TEST_CASE("a retransmit covering a whole hole closes it",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 20));

  std::uint64_t removed = 0;
  REQUIRE(set.fill(range(10, 20)).get(removed) == dfr::error::ok);
  CHECK(removed == 10);
  CHECK(set.empty());
}

TEST_CASE("a partial retransmit leaves the rest outstanding",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 20));

  std::uint64_t removed = 0;
  REQUIRE(set.fill(range(10, 15)).get(removed) == dfr::error::ok);
  CHECK(removed == 5);
  CHECK(listed(set) == std::vector{range(15, 20)});
}

TEST_CASE("a retransmit landing inside a hole splits it",
          "[recovery][gap_set]") {
  // One fill increases the number of outstanding ranges. This is why fill() can run
  // out of capacity even though it is removing data.
  rec::gap_set set;
  must_open(set, range(10, 20));

  std::uint64_t removed = 0;
  REQUIRE(set.fill(range(14, 16)).get(removed) == dfr::error::ok);
  CHECK(removed == 2);
  CHECK(listed(set) == std::vector{range(10, 14), range(16, 20)});
  CHECK(set.total_missing() == 8);
}

TEST_CASE("a fill spanning several holes closes all of them",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 20));
  must_open(set, range(30, 40));
  must_open(set, range(50, 60));

  std::uint64_t removed = 0;
  REQUIRE(set.fill(range(15, 55)).get(removed) == dfr::error::ok);
  CHECK(removed == 5 + 10 + 5);
  CHECK(listed(set) == std::vector{range(10, 15), range(55, 60)});
}

TEST_CASE("a fill nobody was missing removes nothing and is not an error",
          "[recovery][gap_set]") {
  // A duplicate retransmit, or the late half of an A/B pair. Routine, and reported
  // as zero rather than as a failure: the count is the return precisely so this
  // case is distinguishable from a fill that did work.
  rec::gap_set set;
  must_open(set, range(10, 20));

  std::uint64_t removed = 0;
  REQUIRE(set.fill(range(100, 110)).get(removed) == dfr::error::ok);
  CHECK(removed == 0);
  CHECK(set.total_missing() == 10);
}

TEST_CASE("filling an empty set is harmless", "[recovery][gap_set]") {
  rec::gap_set set;
  std::uint64_t removed = 0;
  REQUIRE(set.fill(range(10, 20)).get(removed) == dfr::error::ok);
  CHECK(removed == 0);
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

TEST_CASE("the set fills to capacity and then refuses",
          "[recovery][gap_set]") {
  // TIGER_STYLE: put a limit on everything. Sixteen separate holes on one channel
  // means recovery is losing ground, and accumulating them silently would turn a
  // detectable problem into a slow one.
  rec::gap_set set;
  for (std::uint64_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    // Spaced by two so no two are adjacent and none merge away.
    must_open(set, range(10 + i * 4, 12 + i * 4));
  }
  REQUIRE(set.size() == rec::kMaxOutstandingGaps);

  const auto refused = set.open(range(10'000, 10'010));
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
}

TEST_CASE("a refused open leaves the set describing exactly what it did",
          "[recovery][gap_set][regression]") {
  // The property that makes the refusal usable: open() absorbs before it checks for
  // room, so a failure must not have dropped the ranges it had already absorbed.
  // Reporting an error while quietly forgetting a hole would be worse than
  // accepting one too many.
  rec::gap_set set;
  for (std::uint64_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    must_open(set, range(10 + i * 4, 12 + i * 4));
  }
  const auto before = listed(set);
  const auto total_before = set.total_missing();

  REQUIRE_FALSE(set.open(range(10'000, 10'010)).has_value());

  CHECK(listed(set) == before);
  CHECK(set.total_missing() == total_before);
}

TEST_CASE("an open that merges is accepted even at capacity",
          "[recovery][gap_set]") {
  // Because it needs no new slot. A set at capacity that refused a hole adjacent to
  // one it already holds would be refusing to learn something it can represent for
  // free, and the caller would enter recovery for a range it is already tracking.
  rec::gap_set set;
  for (std::uint64_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    must_open(set, range(10 + i * 4, 12 + i * 4));
  }
  REQUIRE(set.size() == rec::kMaxOutstandingGaps);

  // Adjacent to the first range on its low side, so it touches exactly one.
  CHECK(set.open(range(8, 10)).has_value());
  CHECK(set.size() == rec::kMaxOutstandingGaps);
  CHECK(set.oldest() == range(8, 12));
}

TEST_CASE("a hole bridging two others frees a slot", "[recovery][gap_set]") {
  // The setup spaces holes two apart, so a two-wide hole between any pair is
  // adjacent to *both* and collapses three ranges into one. Worth pinning: it means
  // a set at capacity is not necessarily stuck, and it is the case that made an
  // earlier version of the test above wrong.
  rec::gap_set set;
  for (std::uint64_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    must_open(set, range(10 + i * 4, 12 + i * 4));
  }
  REQUIRE(set.size() == rec::kMaxOutstandingGaps);

  must_open(set, range(12, 14));  // touches (10,12) and (14,16)
  CHECK(set.size() == rec::kMaxOutstandingGaps - 1);
  CHECK(set.oldest() == range(10, 16));
}

TEST_CASE("a fill that would split past capacity is refused, not merged",
          "[recovery][gap_set][regression]") {
  // Reachable because a mid-range fill splits one range into two. Merging the two
  // halves instead would claim the messages between them had arrived, which is the
  // one lie the library exists to catch, so it is refused, and the set is left
  // untouched and still accurate.
  rec::gap_set set;
  for (std::uint64_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    must_open(set, range(10 + i * 10, 19 + i * 10));
  }
  REQUIRE(set.size() == rec::kMaxOutstandingGaps);
  const auto before = listed(set);

  // Lands strictly inside the first range, splitting it and needing a 17th slot.
  const auto refused = set.fill(range(14, 15));
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
  CHECK(listed(set) == before);
}

// ---------------------------------------------------------------------------
// Abandoning holes a snapshot has made unfillable
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot discards holes below it and reports the loss",
          "[recovery][gap_set]") {
  // What a snapshot does: it re-establishes state at a sequence number, so holes
  // older than it can never be filled. The count is returned because abandoning a
  // hole is data loss even when it is the right thing to do, and a component that
  // cannot report how much it gave up cannot be audited.
  rec::gap_set set;
  must_open(set, range(10, 20));
  must_open(set, range(30, 40));
  must_open(set, range(50, 60));

  CHECK(set.discard_below(35) == 10 + 5);
  CHECK(listed(set) == std::vector{range(35, 40), range(50, 60)});
}

TEST_CASE("discarding below everything empties the set",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 20));
  must_open(set, range(30, 40));

  CHECK(set.discard_below(1'000) == 20);
  CHECK(set.empty());
}

TEST_CASE("discarding below the start of the oldest hole changes nothing",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 20));

  CHECK(set.discard_below(10) == 0);
  CHECK(listed(set) == std::vector{range(10, 20)});
}

// ---------------------------------------------------------------------------
// Asking what part of an arrival was actually wanted
// ---------------------------------------------------------------------------

TEST_CASE("intersect names the part of an arrival that was missing",
          "[recovery][gap_set]") {
  // The question the arbiter cannot answer. A retransmit, or the other line's copy
  // arriving late: lands below the merged stream's watermark and looks like a duplicate
  // while being exactly what recovery was waiting for.
  rec::gap_set set;
  must_open(set, range(10, 20));

  CHECK(listed(set.intersect(range(12, 15))) == std::vector{range(12, 15)});
  CHECK(listed(set.intersect(range(0, 100))) == std::vector{range(10, 20)});
  CHECK(listed(set.intersect(range(5, 15))) == std::vector{range(10, 15)});
  CHECK(listed(set.intersect(range(15, 50))) == std::vector{range(15, 20)});
}

TEST_CASE("intersect is empty when nothing in the arrival was wanted",
          "[recovery][gap_set]") {
  rec::gap_set set;
  must_open(set, range(10, 20));

  CHECK(set.intersect(range(50, 60)).empty());
  CHECK(set.intersect(range(20, 30)).empty());  // adjacent, not overlapping
  CHECK(set.intersect(range(7, 7)).empty());
  CHECK(rec::gap_set{}.intersect(range(10, 20)).empty());
}

TEST_CASE("an arrival spanning several holes reports each separately",
          "[recovery][gap_set]") {
  // Which is why the answer is a set and not a range. Reporting the span 12..47 instead
  // would claim the messages between the holes had been missing, and the caller would
  // deliver them twice.
  rec::gap_set set;
  must_open(set, range(10, 20));
  must_open(set, range(30, 40));
  must_open(set, range(45, 50));

  const auto wanted = set.intersect(range(12, 47));
  CHECK(listed(wanted) ==
        std::vector{range(12, 20), range(30, 40), range(45, 47)});
  CHECK(wanted.total_missing() == 8 + 10 + 2);
}

TEST_CASE("intersect leaves the original set untouched",
          "[recovery][gap_set]") {
  // It answers a question; closing the holes is fill()'s job. A client consults it *before*
  // the fill, so an intersect that mutated would make the fill a no-op.
  rec::gap_set set;
  must_open(set, range(10, 20));
  const auto before = listed(set);

  (void)set.intersect(range(12, 15));
  CHECK(listed(set) == before);
}

TEST_CASE("clear forgets everything", "[recovery][gap_set]") {
  // For a session change, where every sequence number held refers to a stream that
  // no longer exists.
  rec::gap_set set;
  must_open(set, range(10, 20));
  set.clear();
  CHECK(set.empty());
  CHECK(set.total_missing() == 0);
}

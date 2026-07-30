// The range arithmetic a gap is made of.
//
// Pure functions with no state, which is why they get their own file: every subtle
// recovery bug further up is ultimately an off-by-one here, and an off-by-one here
// is much cheaper to find in isolation than through a tracker three layers above.

#include <dfr/recovery/gap.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace rec = dfr::recovery;

namespace {

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

}  // namespace

// ---------------------------------------------------------------------------
// The range itself
// ---------------------------------------------------------------------------

TEST_CASE("a range counts the sequences it holds", "[recovery][gap]") {
  CHECK(range(5, 10).count() == 5);
  CHECK(range(5, 6).count() == 1);
  CHECK(range(5, 5).count() == 0);
  CHECK(range(5, 5).empty());
  CHECK_FALSE(range(5, 6).empty());
}

TEST_CASE("a backwards range is empty rather than enormous",
          "[recovery][gap][regression]") {
  // end < first would make end - first wrap to something near 2^64. count() checks
  // empty() first for exactly this reason: a wrapped count would be reported as
  // "18 quintillion messages missing" and, worse, would be believed.
  CHECK(range(10, 5).empty());
  CHECK(range(10, 5).count() == 0);
}

TEST_CASE("last() is the inclusive end, for wire requests",
          "[recovery][gap]") {
  CHECK(range(5, 10).last() == 9);
  CHECK(range(5, 6).last() == 5);
}

TEST_CASE("asking for the last sequence of an empty range aborts",
          "[recovery][gap]") {
  // There is no correct answer, so this is a programmer error rather than a value
  // to return. Asserted, not reported: no wire data can cause it.
  DFR_CHECK_ABORTS({
    const auto r = range(5, 5);
    (void)r.last();
  });
}

TEST_CASE("containment is half-open at both ends", "[recovery][gap]") {
  const auto r = range(5, 10);
  CHECK_FALSE(r.contains(4));
  CHECK(r.contains(5));
  CHECK(r.contains(9));
  CHECK_FALSE(r.contains(10));  // the whole point of half-open
  CHECK_FALSE(range(5, 5).contains(5));
}

// ---------------------------------------------------------------------------
// Relations
// ---------------------------------------------------------------------------

TEST_CASE("covers is inclusive of equality and vacuous for empty",
          "[recovery][gap]") {
  CHECK(range(5, 10).covers(range(5, 10)));
  CHECK(range(5, 10).covers(range(6, 9)));
  CHECK_FALSE(range(5, 10).covers(range(5, 11)));
  CHECK_FALSE(range(5, 10).covers(range(4, 10)));

  // An empty range is covered by anything, including another empty range. This is
  // what lets a fill loop terminate without special-casing the last step.
  CHECK(range(5, 10).covers(range(7, 7)));
  CHECK(range(5, 5).covers(range(7, 7)));
  CHECK_FALSE(range(5, 5).covers(range(5, 6)));
}

TEST_CASE("touching at a boundary is adjacency, not overlap",
          "[recovery][gap]") {
  // The distinction that makes canonical form possible: 5..9 and 10..14 have no
  // message between them, so they are one hole, but they share no sequence number.
  CHECK(range(5, 10).adjacent_to(range(10, 15)));
  CHECK(range(10, 15).adjacent_to(range(5, 10)));
  CHECK_FALSE(range(5, 10).overlaps(range(10, 15)));

  CHECK(range(5, 10).overlaps(range(9, 15)));
  CHECK_FALSE(range(5, 10).adjacent_to(range(11, 15)));  // 10 is missing
  CHECK_FALSE(range(5, 10).overlaps(range(11, 15)));
}

TEST_CASE("an empty range is neither adjacent nor overlapping",
          "[recovery][gap]") {
  // Otherwise an empty range at 10 would be "adjacent" to everything ending at 10
  // and merging it would look meaningful.
  CHECK_FALSE(range(5, 10).adjacent_to(range(10, 10)));
  CHECK_FALSE(range(5, 10).overlaps(range(7, 7)));
}

// ---------------------------------------------------------------------------
// merge
// ---------------------------------------------------------------------------

TEST_CASE("merging joins overlapping and adjacent ranges",
          "[recovery][gap]") {
  CHECK(merge(range(5, 10), range(10, 15)) == range(5, 15));
  CHECK(merge(range(5, 12), range(8, 15)) == range(5, 15));
  CHECK(merge(range(5, 20), range(8, 9)) == range(5, 20));
  CHECK(merge(range(10, 15), range(5, 10)) == range(5, 15));  // order-independent
}

TEST_CASE("merging with an empty range yields the other one",
          "[recovery][gap]") {
  CHECK(merge(range(5, 10), range(7, 7)) == range(5, 10));
  CHECK(merge(range(7, 7), range(5, 10)) == range(5, 10));
}

TEST_CASE("merging disjoint ranges aborts", "[recovery][gap]") {
  // The union of 5..9 and 20..24 would be 5..24, which claims that 10 through 19
  // arrived. That is precisely the lie this library exists to catch, so it is a
  // programmer error rather than a permitted rounding.
  DFR_CHECK_ABORTS((void)merge(range(5, 10), range(20, 25)));
}

// ---------------------------------------------------------------------------
// subtract
// ---------------------------------------------------------------------------

TEST_CASE("a fill covering the whole hole leaves nothing",
          "[recovery][gap]") {
  const auto left = subtract(range(5, 10), range(5, 10));
  CHECK(left.empty());
  CHECK(left.count() == 0);
}

TEST_CASE("a fill at the front trims the front", "[recovery][gap]") {
  const auto left = subtract(range(5, 10), range(5, 7));
  CHECK(left.before.empty());
  CHECK(left.after == range(7, 10));
  CHECK(left.count() == 3);
}

TEST_CASE("a fill at the back trims the back", "[recovery][gap]") {
  const auto left = subtract(range(5, 10), range(8, 10));
  CHECK(left.before == range(5, 8));
  CHECK(left.after.empty());
  CHECK(left.count() == 3);
}

TEST_CASE("a fill in the middle splits the hole in two",
          "[recovery][gap]") {
  // The case that makes remainder two ranges instead of one. Asking for 5..9 and
  // receiving only 7 leaves 5..6 and 8..9 missing; a subtract returning one range
  // would have to round outward and claim messages arrived that did not.
  const auto left = subtract(range(5, 10), range(7, 8));
  CHECK(left.before == range(5, 7));
  CHECK(left.after == range(8, 10));
  CHECK(left.count() == 4);
}

TEST_CASE("a fill that misses the hole entirely changes nothing",
          "[recovery][gap]") {
  const auto left = subtract(range(5, 10), range(20, 25));
  CHECK(left.before == range(5, 10));
  CHECK(left.after.empty());
  CHECK(left.count() == 5);
}

TEST_CASE("a fill wider than the hole still leaves nothing",
          "[recovery][gap]") {
  // A retransmit is allowed to be generous. Over-delivering must not underflow the
  // remainder into a wrapped range.
  const auto left = subtract(range(5, 10), range(0, 100));
  CHECK(left.empty());
}

TEST_CASE("subtracting from or by an empty range is a no-op",
          "[recovery][gap]") {
  CHECK(subtract(range(5, 5), range(1, 100)).empty());
  CHECK(subtract(range(5, 10), range(7, 7)).before == range(5, 10));
}

TEST_CASE("the range arithmetic is available at compile time",
          "[recovery][gap]") {
  // Everything here is constexpr, so a test can be a static_assert and a bug in the
  // arithmetic can be a build failure rather than a test run.
  static_assert(range(5, 10).count() == 5);
  static_assert(range(10, 5).count() == 0);
  static_assert(range(5, 10).adjacent_to(range(10, 15)));
  static_assert(merge(range(5, 10), range(10, 15)) == range(5, 15));
  static_assert(subtract(range(5, 10), range(7, 8)).after == range(8, 10));
  SUCCEED("the static_asserts above are the test");
}

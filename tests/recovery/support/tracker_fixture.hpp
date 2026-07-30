// Shared setup for the gap_tracker tests.
//
// Two files drive one tracker — the sequencing outcomes, and the events that throw
// state away — and they need the same feeder and the same view of what is missing.
// Here rather than copied, per docs/STYLE.md §1.10.
//
// `inline` on the free functions, because more than one translation unit includes
// this header.

#ifndef DFR_TESTS_RECOVERY_SUPPORT_TRACKER_FIXTURE_HPP
#define DFR_TESTS_RECOVERY_SUPPORT_TRACKER_FIXTURE_HPP

#include <dfr/recovery/gap_tracker.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace dfr_test::recovery {

namespace rec = dfr::recovery;

inline constexpr std::uint32_t kSession = 0xABCD;
inline constexpr rec::channel_id kChannel = rec::channel_id::at(0);

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

// Feeds a packet and requires it to be accepted, returning what happened. Failure
// here means the tracker ran out of capacity, which every test but the capacity ones
// treats as a bug in the test.
inline rec::observation feed(rec::gap_tracker& tracker, std::uint64_t first,
                             std::uint64_t count,
                             std::uint32_t session = kSession) {
  rec::observation out;
  REQUIRE(tracker.observe(kChannel, session, first, count).get(out) ==
          dfr::error::ok);
  return out;
}

// A copy, not the span: outstanding() hands back a reference into the tracker and
// ranges() carries DFR_LIFETIME_BOUND for the reason ASan already demonstrated once.
inline std::vector<rec::sequence_range> missing(const rec::gap_tracker& tracker) {
  const auto& set = tracker.outstanding(kChannel);
  return {set.ranges().begin(), set.ranges().end()};
}

}  // namespace dfr_test::recovery

#endif  // DFR_TESTS_RECOVERY_SUPPORT_TRACKER_FIXTURE_HPP

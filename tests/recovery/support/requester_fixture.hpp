// Shared setup for the requester tests.
//
// The requester never reads a clock, so every test drives time explicitly. A
// manual_clock is still the natural source of the time points — it makes the units
// obvious and keeps the tests reading like a timeline.

#ifndef DFR_TESTS_RECOVERY_SUPPORT_REQUESTER_FIXTURE_HPP
#define DFR_TESTS_RECOVERY_SUPPORT_REQUESTER_FIXTURE_HPP

#include <dfr/recovery/requester.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace dfr_test::recovery {

namespace rec = dfr::recovery;

using test_requester = rec::requester<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

// A policy with round numbers, so a test asserts on a timeline a reader can follow:
// settle 1ms, then 10ms, 20ms, 40ms between attempts, three attempts, one second of
// retention.
inline rec::retransmit_policy readable_policy() {
  rec::retransmit_policy policy;
  policy.settle_delay = std::chrono::milliseconds{1};
  policy.first_timeout = std::chrono::milliseconds{10};
  policy.max_timeout = std::chrono::milliseconds{500};
  policy.backoff_numerator = 2;
  policy.backoff_denominator = 1;
  policy.max_attempts = 3;
  policy.retention_window = std::chrono::seconds{1};
  REQUIRE(policy.validate().has_value());
  return policy;
}

inline test_time at_ms(std::int64_t millis) {
  return test_time{} + std::chrono::milliseconds{millis};
}

// Registers a hole and requires it to be accepted, for setup steps whose failure would
// be a bug in the test rather than the point of it.
inline void must_open(test_requester& requester, rec::sequence_range hole,
                      test_time now) {
  REQUIRE(requester.on_gap(hole, now).has_value());
}

}  // namespace dfr_test::recovery

#endif  // DFR_TESTS_RECOVERY_SUPPORT_REQUESTER_FIXTURE_HPP

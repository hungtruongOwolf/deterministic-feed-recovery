// Shared setup for the schedule tests.
//
// Generating a schedule from a seed and flattening one into a vector are needed by
// both the generation tests and the shrinking tests. Here rather than copied, per
// docs/STYLE.md §1.10.
//
// `inline` on both, because more than one translation unit includes this header.

#ifndef DFR_TESTS_CHAOS_SUPPORT_SCHEDULE_FIXTURE_HPP
#define DFR_TESTS_CHAOS_SUPPORT_SCHEDULE_FIXTURE_HPP

#include <dfr/chaos/schedule.hpp>
#include <dfr/core/rng.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace dfr_test::chaos {

namespace chaos = dfr::chaos;

inline chaos::schedule generate(std::uint64_t seed,
                                const chaos::schedule_options& options = {},
                                std::uint64_t stream = 1'000) {
  dfr::prng rng{seed};
  chaos::schedule out;
  REQUIRE(chaos::schedule::generate(rng, options, stream).get(out) ==
          dfr::error::ok);
  return out;
}

// A copy, not the span: schedule::faults() carries DFR_LIFETIME_BOUND precisely
// because iterating the span of a temporary is a use-after-scope, which is how
// ASan found that bug in the first place.
inline std::vector<chaos::fault> entries(const chaos::schedule& s) {
  return {s.faults().begin(), s.faults().end()};
}

}  // namespace dfr_test::chaos

#endif  // DFR_TESTS_CHAOS_SUPPORT_SCHEDULE_FIXTURE_HPP

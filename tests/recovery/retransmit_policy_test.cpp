// The retransmit policy: what a legal configuration is, and how the backoff grows.
//
// Worth testing apart from the state machine because these are the numbers an
// operator changes, and every one of them has a way of failing that looks like a
// different bug entirely.

#include <dfr/recovery/retransmit_policy.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace rec = dfr::recovery;

using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST_CASE("the default policy is legal", "[recovery][policy]") {
  // Defaults are load-bearing: docs/DESIGN.md quotes TIGER_STYLE on passing options
  // explicitly, but a default that does not validate would make the type unusable
  // until fully specified and would push everyone into copy-pasting a configuration.
  CHECK(rec::retransmit_policy{}.validate().has_value());
}

TEST_CASE("a zero backoff denominator is rejected", "[recovery][policy]") {
  // It would divide by zero inside timeout_for(), on the recovery path, only for the
  // operator who tuned the backoff.
  rec::retransmit_policy policy;
  policy.backoff_denominator = 0;
  CHECK(policy.validate().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a shrinking backoff is rejected", "[recovery][policy]") {
  // A ratio below one would make a struggling receiver ask *faster* the longer it
  // failed — the opposite of backoff, and a recipe for the storm backoff exists to
  // prevent. Cheap to catch here and hard to spot in production.
  rec::retransmit_policy policy;
  policy.backoff_numerator = 1;
  policy.backoff_denominator = 2;
  CHECK(policy.validate().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("zero attempts is rejected", "[recovery][policy]") {
  // A hole would be abandoned before it was ever requested, and the receiver would
  // report unrecoverable loss having sent nothing.
  rec::retransmit_policy policy;
  policy.max_attempts = 0;
  CHECK(policy.validate().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a request size of zero is rejected", "[recovery][policy]") {
  // on_gap() chunks by this number; zero would not terminate.
  rec::retransmit_policy policy;
  policy.max_messages_per_request = 0;
  CHECK(policy.validate().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a retention window shorter than the first attempt is rejected",
          "[recovery][policy]") {
  // The first request would already be too late to be worth sending, so every gap
  // would be abandoned unrecoverable while the retransmit server sat idle.
  rec::retransmit_policy policy;
  policy.settle_delay = milliseconds{10};
  policy.first_timeout = milliseconds{50};
  policy.retention_window = milliseconds{60};  // exactly the sum, so still too short
  CHECK(policy.validate().error_code() == dfr::error::invalid_argument);

  policy.retention_window = milliseconds{61};
  CHECK(policy.validate().has_value());
}

TEST_CASE("a max timeout below the first timeout is rejected",
          "[recovery][policy]") {
  rec::retransmit_policy policy;
  policy.first_timeout = milliseconds{100};
  policy.max_timeout = milliseconds{50};
  CHECK(policy.validate().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a non-positive first timeout is rejected", "[recovery][policy]") {
  rec::retransmit_policy policy;
  policy.first_timeout = dfr::duration::zero();
  CHECK(policy.validate().error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// Backoff
// ---------------------------------------------------------------------------

TEST_CASE("the first attempt waits the first timeout", "[recovery][policy]") {
  rec::retransmit_policy policy;
  policy.first_timeout = milliseconds{50};
  CHECK(policy.timeout_for(1) == dfr::duration{milliseconds{50}});
}

TEST_CASE("doubling is the default and grows as expected",
          "[recovery][policy]") {
  rec::retransmit_policy policy;
  policy.first_timeout = milliseconds{10};
  policy.max_timeout = seconds{10};
  policy.backoff_numerator = 2;
  policy.backoff_denominator = 1;
  REQUIRE(policy.validate().has_value());

  CHECK(policy.timeout_for(1) == dfr::duration{milliseconds{10}});
  CHECK(policy.timeout_for(2) == dfr::duration{milliseconds{20}});
  CHECK(policy.timeout_for(3) == dfr::duration{milliseconds{40}});
  CHECK(policy.timeout_for(4) == dfr::duration{milliseconds{80}});
}

TEST_CASE("a fractional ratio backs off gently", "[recovery][policy]") {
  // 3/2, computed as integers. The point of the integer ratio is that this sequence
  // is exactly reproducible: no rounding decision sits inside the replay path.
  rec::retransmit_policy policy;
  policy.first_timeout = milliseconds{100};
  policy.max_timeout = seconds{10};
  policy.backoff_numerator = 3;
  policy.backoff_denominator = 2;
  REQUIRE(policy.validate().has_value());

  CHECK(policy.timeout_for(1) == dfr::duration{milliseconds{100}});
  CHECK(policy.timeout_for(2) == dfr::duration{milliseconds{150}});
  CHECK(policy.timeout_for(3) == dfr::duration{milliseconds{225}});
}

TEST_CASE("a ratio of one to one disables backoff", "[recovery][policy]") {
  rec::retransmit_policy policy;
  policy.first_timeout = milliseconds{25};
  policy.backoff_numerator = 1;
  policy.backoff_denominator = 1;
  REQUIRE(policy.validate().has_value());

  CHECK(policy.timeout_for(1) == dfr::duration{milliseconds{25}});
  CHECK(policy.timeout_for(5) == dfr::duration{milliseconds{25}});
  CHECK(policy.timeout_for(500) == dfr::duration{milliseconds{25}});
}

TEST_CASE("the backoff stops at the ceiling and stays there",
          "[recovery][policy]") {
  // The ceiling is not cosmetic: without it a doubling backoff pushes the next attempt
  // past the retention window, turning a recoverable gap into a snapshot because the
  // receiver decided to be patient.
  rec::retransmit_policy policy;
  policy.first_timeout = milliseconds{10};
  policy.max_timeout = milliseconds{100};
  REQUIRE(policy.validate().has_value());

  CHECK(policy.timeout_for(4) == dfr::duration{milliseconds{80}});
  CHECK(policy.timeout_for(5) == dfr::duration{milliseconds{100}});
  CHECK(policy.timeout_for(6) == dfr::duration{milliseconds{100}});
}

TEST_CASE("a huge attempt number cannot wrap into a short timeout",
          "[recovery][policy][regression]") {
  // The failure this saturation prevents is the worst possible one: a backoff that
  // overflowed would produce a *tiny* timeout, so the receiver would hammer the
  // retransmit server hardest at the exact moment it had already been failing for a
  // long time.
  rec::retransmit_policy policy;
  policy.first_timeout = milliseconds{1};
  policy.max_timeout = seconds{2};
  REQUIRE(policy.validate().has_value());

  for (std::uint32_t attempt = 1; attempt < 5'000; ++attempt) {
    const auto wait = policy.timeout_for(attempt);
    CHECK(wait > dfr::duration::zero());
    CHECK(wait <= dfr::duration{seconds{2}});
  }
}

TEST_CASE("attempt zero is a programmer error", "[recovery][policy]") {
  // Attempts are counted from one, so asking about attempt zero means the caller has
  // an off-by-one and would silently receive the first timeout.
  DFR_CHECK_ABORTS((void)rec::retransmit_policy{}.timeout_for(0));
}

TEST_CASE("the policy is usable at compile time", "[recovery][policy]") {
  static_assert(rec::retransmit_policy{}.validate().has_value());
  static_assert(rec::retransmit_policy{.first_timeout = milliseconds{10}}
                    .timeout_for(3) == dfr::duration{milliseconds{40}});
  SUCCEED("the static_asserts above are the test");
}

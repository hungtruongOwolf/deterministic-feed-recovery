// The requester's timeline: when it asks, when it waits, and when it gives up.
//
// Every test here is a sequence of poll() calls at explicit instants, because that is
// exactly how the component is meant to be used and because a bug in a retry timeline
// is otherwise only visible under load in production.
//
// Chunking, merging and partial fills are in requester_chunking_test.cpp — those are
// about *what* is asked for rather than *when*.

#include <dfr/recovery/requester.hpp>

#include "recovery/support/requester_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace rec = dfr::recovery;

using dfr_test::recovery::at_ms;
using dfr_test::recovery::must_open;
using dfr_test::recovery::range;
using dfr_test::recovery::readable_policy;
using dfr_test::recovery::test_requester;

// ---------------------------------------------------------------------------
// Suppression: not asking is the first thing it must get right
// ---------------------------------------------------------------------------

TEST_CASE("a requester with nothing outstanding is idle",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  CHECK(requester.poll(at_ms(0)).what == rec::action::idle);
  CHECK(requester.pending() == 0);
  CHECK(requester.stats().requests_sent == 0);
}

TEST_CASE("a fresh gap is not requested immediately",
          "[recovery][requester]") {
  // The substance of NAK suppression. Most gaps on a multicast feed are transient
  // reordering, and a receiver that asks the instant it notices generates recovery
  // traffic for data already in flight — simultaneously, from every receiver on the
  // group, for the same packet.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));

  CHECK(requester.poll(at_ms(0)).what == rec::action::idle);
  CHECK(requester.pending() == 1);
  CHECK(requester.messages_outstanding() == 10);
  CHECK(requester.stats().requests_sent == 0);
}

TEST_CASE("a gap that fills during the settle delay is never requested",
          "[recovery][requester]") {
  // The case that justifies the delay existing. A packet arriving 500 microseconds
  // late costs nothing; asking for it costs a round trip and a server's attention.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));
  REQUIRE(requester.on_filled(range(10, 20)).has_value());

  CHECK(requester.poll(at_ms(100)).what == rec::action::idle);
  CHECK(requester.pending() == 0);
  CHECK(requester.stats().requests_sent == 0);
}

TEST_CASE("the first request goes out once the delay has passed",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));

  const auto sent = requester.poll(at_ms(1));
  CHECK(sent.what == rec::action::send);
  CHECK(sent.range == range(10, 20));
  CHECK(sent.attempt == 1);
  CHECK(sent.reason == dfr::error::ok);
  CHECK(requester.stats().requests_sent == 1);
}

TEST_CASE("polling again before the timeout asks for nothing",
          "[recovery][requester]") {
  // Suppression while a request is in flight. Without it the caller's poll loop rate
  // becomes the request rate, which is how a recovery client turns one lost packet
  // into thousands of requests.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));
  REQUIRE(requester.poll(at_ms(1)).what == rec::action::send);

  for (std::int64_t t = 1; t < 11; ++t) {
    CHECK(requester.poll(at_ms(t)).what == rec::action::idle);
  }
  CHECK(requester.stats().requests_sent == 1);
}

// ---------------------------------------------------------------------------
// Retrying, with backoff
// ---------------------------------------------------------------------------

TEST_CASE("an unanswered request is retried after its timeout",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));
  REQUIRE(requester.poll(at_ms(1)).what == rec::action::send);

  const auto second = requester.poll(at_ms(11));
  CHECK(second.what == rec::action::send);
  CHECK(second.attempt == 2);
  CHECK(second.range == range(10, 20));
}

TEST_CASE("each retry waits longer than the last", "[recovery][requester]") {
  // The timeline the readable policy describes: settle 1ms, then 10ms, then 20ms.
  // Asserting the instants rather than the durations, because an off-by-one in the
  // comparison is what actually goes wrong and it is invisible in a duration.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));

  REQUIRE(requester.poll(at_ms(1)).attempt == 1);
  CHECK(requester.poll(at_ms(10)).what == rec::action::idle);   // 1 + 10 = 11
  REQUIRE(requester.poll(at_ms(11)).attempt == 2);
  CHECK(requester.poll(at_ms(30)).what == rec::action::idle);   // 11 + 20 = 31
  REQUIRE(requester.poll(at_ms(31)).attempt == 3);
  CHECK(requester.stats().requests_sent == 3);
}

TEST_CASE("a reply stops the retries", "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));
  REQUIRE(requester.poll(at_ms(1)).attempt == 1);

  REQUIRE(requester.on_filled(range(10, 20)).has_value());
  CHECK(requester.poll(at_ms(11)).what == rec::action::idle);
  CHECK(requester.pending() == 0);
  CHECK(requester.stats().requests_sent == 1);
}

// ---------------------------------------------------------------------------
// Giving up
// ---------------------------------------------------------------------------

TEST_CASE("the last attempt is given its full timeout before giving up",
          "[recovery][requester][regression]") {
  // Abandoning the moment the final request is sent would throw away a range whose
  // reply was still in flight — and the reply would then arrive for a range the
  // requester had already declared unrecoverable.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));

  REQUIRE(requester.poll(at_ms(1)).attempt == 1);
  REQUIRE(requester.poll(at_ms(11)).attempt == 2);
  REQUIRE(requester.poll(at_ms(31)).attempt == 3);  // the last one, waits 40ms

  CHECK(requester.poll(at_ms(50)).what == rec::action::idle);
  CHECK(requester.poll(at_ms(70)).what == rec::action::idle);

  const auto given_up = requester.poll(at_ms(71));
  CHECK(given_up.what == rec::action::abandon);
  CHECK(given_up.range == range(10, 20));
  CHECK(given_up.reason == dfr::error::retransmit_timed_out);
}

TEST_CASE("an abandoned range is dropped and not reported twice",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));
  for (const std::int64_t t : {1, 11, 31}) {
    REQUIRE(requester.poll(at_ms(t)).what == rec::action::send);
  }
  REQUIRE(requester.poll(at_ms(71)).what == rec::action::abandon);

  CHECK(requester.poll(at_ms(72)).what == rec::action::idle);
  CHECK(requester.pending() == 0);
  CHECK(requester.stats().ranges_abandoned == 1);
  CHECK(requester.stats().messages_abandoned == 10);
  CHECK(requester.stats().timed_out == 1);
}

TEST_CASE("the retention window gives up even with attempts to spare",
          "[recovery][requester]") {
  // The window is a fact about the publisher, not about our persistence: once the data
  // has aged out of its buffer, no number of remaining attempts can recover it, and
  // continuing to ask is pure noise.
  auto policy = readable_policy();
  policy.max_attempts = 100;
  policy.retention_window = std::chrono::milliseconds{200};
  REQUIRE(policy.validate().has_value());

  test_requester requester{policy};
  must_open(requester, range(10, 20), at_ms(0));
  REQUIRE(requester.poll(at_ms(1)).attempt == 1);

  const auto given_up = requester.poll(at_ms(200));
  CHECK(given_up.what == rec::action::abandon);
  CHECK(given_up.reason == dfr::error::retransmit_window_exceeded);
  CHECK(dfr::is_fatal(given_up.reason));  // a snapshot is the only repair left
  CHECK(requester.stats().window_exceeded == 1);
}

TEST_CASE("the window is measured from discovery, not from the last attempt",
          "[recovery][requester][regression]") {
  // Measuring from the last attempt would let a persistent receiver keep a range alive
  // indefinitely by continuing to ask about it, long after the publisher had dropped
  // it — asking forever for something that can never arrive.
  auto policy = readable_policy();
  policy.max_attempts = 100;
  policy.retention_window = std::chrono::milliseconds{100};
  REQUIRE(policy.validate().has_value());

  test_requester requester{policy};
  must_open(requester, range(10, 20), at_ms(0));

  // Keep asking right up to the window.
  REQUIRE(requester.poll(at_ms(1)).attempt == 1);
  REQUIRE(requester.poll(at_ms(11)).attempt == 2);
  REQUIRE(requester.poll(at_ms(31)).attempt == 3);
  REQUIRE(requester.poll(at_ms(71)).attempt == 4);

  CHECK(requester.poll(at_ms(100)).what == rec::action::abandon);
}

// ---------------------------------------------------------------------------
// Priority
// ---------------------------------------------------------------------------

TEST_CASE("the oldest hole is requested first", "[recovery][requester]") {
  // It is the one closest to falling out of the retention window, so spending an
  // attempt on a younger range first spends it on the one that could have waited.
  test_requester requester{readable_policy()};
  must_open(requester, range(100, 110), at_ms(0));
  must_open(requester, range(200, 210), at_ms(1));

  CHECK(requester.poll(at_ms(5)).range == range(100, 110));
  CHECK(requester.poll(at_ms(5)).range == range(200, 210));
  CHECK(requester.poll(at_ms(5)).what == rec::action::idle);
}

TEST_CASE("abandonment takes priority over sending",
          "[recovery][requester]") {
  // A caller that only handles one decision per iteration must hear the bad news
  // first: a range that is already unrecoverable should not delay a snapshot while
  // requests go out for a younger one.
  auto policy = readable_policy();
  policy.max_attempts = 100;
  policy.retention_window = std::chrono::milliseconds{50};
  REQUIRE(policy.validate().has_value());

  test_requester requester{policy};
  must_open(requester, range(100, 110), at_ms(0));
  must_open(requester, range(200, 210), at_ms(45));

  const auto first = requester.poll(at_ms(50));
  CHECK(first.what == rec::action::abandon);
  CHECK(first.range == range(100, 110));

  const auto second = requester.poll(at_ms(50));
  CHECK(second.what == rec::action::send);
  CHECK(second.range == range(200, 210));
}

TEST_CASE("every action has a distinct name", "[recovery][requester]") {
  CHECK(rec::name_of(rec::action::idle) == "idle");
  CHECK(rec::name_of(rec::action::send) == "send");
  CHECK(rec::name_of(rec::action::abandon) == "abandon");
}

// What the requester asks for: chunking to the wire limit, merging, partial replies.
//
// Separate from requester_test.cpp, which is about *when* it asks. These tests barely
// move time at all; they are about the shape of the ranges.

#include <dfr/recovery/requester.hpp>

#include "recovery/support/requester_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace rec = dfr::recovery;

using dfr_test::recovery::at_ms;
using dfr_test::recovery::must_open;
using dfr_test::recovery::range;
using dfr_test::recovery::readable_policy;
using dfr_test::recovery::test_requester;

namespace {

// Drains every request due at one instant, so a test can assert on the whole set of
// requests a large hole produces rather than on one at a time.
std::vector<rec::sequence_range> drain(test_requester& requester,
                                      dfr::manual_clock::time_point now) {
  std::vector<rec::sequence_range> out;
  for (;;) {
    const auto next = requester.poll(now);
    if (next.what != rec::action::send) {
      break;
    }
    out.push_back(next.range);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Chunking to the wire limit
// ---------------------------------------------------------------------------

TEST_CASE("a hole within the request limit is one request",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(0, 60'000), at_ms(0));

  CHECK(requester.pending() == 1);
  CHECK(drain(requester, at_ms(1)) ==
        std::vector{range(0, 60'000)});
}

TEST_CASE("a hole wider than the request limit is chunked",
          "[recovery][requester]") {
  // MoldUDP64's Message Count is 16 bits and the specification caps a request at
  // 60,000. A server asked for more answers nothing at all, so the client would get
  // silence and then report a timeout it caused itself.
  test_requester requester{readable_policy()};
  must_open(requester, range(0, 150'000), at_ms(0));

  CHECK(requester.pending() == 3);
  CHECK(requester.messages_outstanding() == 150'000);
  CHECK(drain(requester, at_ms(1)) ==
        std::vector{range(0, 60'000), range(60'000, 120'000),
                    range(120'000, 150'000)});
}

TEST_CASE("chunking happens at discovery, not at send time",
          "[recovery][requester][regression]") {
  // Clamping the range when the request goes out looks equivalent and is not: the
  // pending entry would still describe the whole hole, so every attempt would
  // re-request the same first 60,000 messages and the tail would never be asked for.
  // The tell is that the second and third attempts move on.
  test_requester requester{readable_policy()};
  must_open(requester, range(0, 150'000), at_ms(0));

  const auto first_round = drain(requester, at_ms(1));
  REQUIRE(first_round.size() == 3);
  CHECK(first_round[1].first == 60'000);
  CHECK(first_round[2].first == 120'000);

  // And the retry round asks for the same three chunks, not three copies of the first.
  const auto second_round = drain(requester, at_ms(11));
  CHECK(second_round == first_round);
}

TEST_CASE("a chunked hole is covered exactly once",
          "[recovery][requester]") {
  // No overlap and no hole between chunks: overlapping would ask for messages twice,
  // and a hole between them would leave messages nobody ever asks for.
  test_requester requester{readable_policy()};
  must_open(requester, range(7, 7 + 250'000), at_ms(0));

  const auto chunks = drain(requester, at_ms(1));
  REQUIRE_FALSE(chunks.empty());
  CHECK(chunks.front().first == 7);
  CHECK(chunks.back().end == 7 + 250'000);
  for (std::size_t i = 1; i < chunks.size(); ++i) {
    CHECK(chunks[i].first == chunks[i - 1].end);
  }
}

// ---------------------------------------------------------------------------
// Merging
// ---------------------------------------------------------------------------

TEST_CASE("adjacent holes become one request", "[recovery][requester]") {
  // One lost burst is one request. A receiver sending one per lost packet multiplies
  // its own recovery traffic by the burst length.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));
  must_open(requester, range(20, 30), at_ms(0));

  CHECK(requester.pending() == 1);
  CHECK(drain(requester, at_ms(1)) == std::vector{range(10, 30)});
}

TEST_CASE("a merge restarts the attempt count", "[recovery][requester]") {
  // Merging produces a request nobody has made yet, so carrying the old attempt count
  // forward would spend the new range's attempts on requests that never covered it.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));
  REQUIRE(requester.poll(at_ms(1)).attempt == 1);
  REQUIRE(requester.poll(at_ms(11)).attempt == 2);

  must_open(requester, range(20, 30), at_ms(12));
  const auto after = requester.poll(at_ms(13));
  CHECK(after.range == range(10, 30));
  CHECK(after.attempt == 1);
}

TEST_CASE("a merge keeps the earlier discovery time",
          "[recovery][requester][regression]") {
  // The retention window is a fact about the age of the data, so it belongs to the
  // oldest byte in the range. Taking the newer time would let a receiver keep
  // refreshing a range's deadline by discovering an adjacent hole next to it, and it
  // would go on asking for messages the publisher no longer has.
  auto policy = readable_policy();
  policy.max_attempts = 100;
  policy.retention_window = std::chrono::milliseconds{100};
  REQUIRE(policy.validate().has_value());

  test_requester requester{policy};
  must_open(requester, range(10, 20), at_ms(0));
  must_open(requester, range(20, 30), at_ms(90));
  REQUIRE(requester.pending() == 1);

  const auto given_up = requester.poll(at_ms(100));
  CHECK(given_up.what == rec::action::abandon);
  CHECK(given_up.range == range(10, 30));
  CHECK(given_up.reason == dfr::error::retransmit_window_exceeded);
}

TEST_CASE("holes are kept apart when merging would exceed the request limit",
          "[recovery][requester]") {
  // Otherwise the merge would produce a range no single request can carry, and the
  // whole point of chunking at discovery would be undone one merge later.
  test_requester requester{readable_policy()};
  must_open(requester, range(0, 40'000), at_ms(0));
  must_open(requester, range(40'000, 80'000), at_ms(0));

  CHECK(requester.pending() == 2);
  for (const auto& asked : drain(requester, at_ms(1))) {
    CHECK(asked.count() <= 60'000);
  }
}

// ---------------------------------------------------------------------------
// Partial replies
// ---------------------------------------------------------------------------

TEST_CASE("a partial reply shrinks what is still asked for",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 30), at_ms(0));
  REQUIRE(requester.poll(at_ms(1)).attempt == 1);

  REQUIRE(requester.on_filled(range(10, 20)).has_value());
  CHECK(requester.messages_outstanding() == 10);
  CHECK(drain(requester, at_ms(11)) == std::vector{range(20, 30)});
}

TEST_CASE("a partial reply does not restart the attempt count",
          "[recovery][requester]") {
  // Progress on the same request. Resetting would let a range that keeps getting
  // one-message replies be retried forever, which is a livelock dressed as diligence.
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 30), at_ms(0));
  REQUIRE(requester.poll(at_ms(1)).attempt == 1);

  REQUIRE(requester.on_filled(range(10, 11)).has_value());
  CHECK(requester.poll(at_ms(11)).attempt == 2);
}

TEST_CASE("a reply landing in the middle splits the request in two",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 30), at_ms(0));

  REQUIRE(requester.on_filled(range(18, 22)).has_value());
  CHECK(requester.pending() == 2);
  CHECK(drain(requester, at_ms(1)) ==
        std::vector{range(10, 18), range(22, 30)});
}

TEST_CASE("a reply for something nobody asked for changes nothing",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  must_open(requester, range(10, 20), at_ms(0));

  REQUIRE(requester.on_filled(range(500, 600)).has_value());
  CHECK(requester.pending() == 1);
  CHECK(requester.messages_outstanding() == 10);
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

TEST_CASE("too many separate holes is reported",
          "[recovery][requester]") {
  test_requester requester{readable_policy()};
  for (std::uint64_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    must_open(requester, range(10 + i * 10, 15 + i * 10), at_ms(0));
  }
  REQUIRE(requester.pending() == rec::kMaxOutstandingGaps);

  const auto refused = requester.on_gap(range(10'000, 10'005), at_ms(0));
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
}

TEST_CASE("a reply that would split past capacity is refused",
          "[recovery][requester]") {
  // Reachable because a mid-range reply splits an entry. Approximating instead would
  // either keep asking for messages that already arrived or stop asking for messages
  // that never came.
  test_requester requester{readable_policy()};
  for (std::uint64_t i = 0; i < rec::kMaxOutstandingGaps; ++i) {
    must_open(requester, range(10 + i * 20, 20 + i * 20), at_ms(0));
  }
  REQUIRE(requester.pending() == rec::kMaxOutstandingGaps);

  const auto refused = requester.on_filled(range(14, 15));
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
  CHECK(requester.pending() == rec::kMaxOutstandingGaps);
}

TEST_CASE("a hole too wide to describe in chunks is reported",
          "[recovery][requester]") {
  // Sixteen chunks of 60,000 is 960,000 messages. Beyond that the receiver is not
  // recovering, it is rebuilding, and it should be told so rather than quietly asking
  // for a prefix and forgetting the rest.
  test_requester requester{readable_policy()};
  const auto refused = requester.on_gap(range(0, 60'000ULL * 20), at_ms(0));
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
}

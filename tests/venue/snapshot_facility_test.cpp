// The snapshot facility, including its ability to lose the race on purpose.

#include <dfr/venue/snapshot_facility.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace venue = dfr::venue;

namespace {

using test_facility = venue::snapshot_facility<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

test_time at_ms(std::int64_t millis) {
  return test_time{} + std::chrono::milliseconds{millis};
}

venue::snapshot_options readable_options() {
  venue::snapshot_options options;
  options.session = 0xFEED;
  options.latency = std::chrono::milliseconds{50};
  REQUIRE(options.validate().has_value());
  return options;
}

}  // namespace

TEST_CASE("the default snapshot options are legal", "[venue][snapshot]") {
  CHECK(venue::snapshot_options{}.validate().has_value());
}

TEST_CASE("a negative latency is rejected", "[venue][snapshot]") {
  venue::snapshot_options options;
  options.latency = dfr::duration::zero() - std::chrono::milliseconds{1};
  CHECK(options.validate().error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// The ordinary path
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot before anything is published is refused",
          "[venue][snapshot]") {
  // There is no state to snapshot. Distinct from a stale reply: the client is early rather
  // than unlucky, and answering with a plausible position would invent one.
  test_facility facility{readable_options()};
  const auto refused = facility.request(at_ms(0));
  CHECK(refused.error_code() == dfr::error::not_supported);
  CHECK(facility.stats().refused_before_feed == 1);
}

TEST_CASE("a reply arrives only after the latency has elapsed",
          "[venue][snapshot]") {
  test_facility facility{readable_options()};
  facility.advance_to(1'000);
  REQUIRE(facility.request(at_ms(0)).has_value());

  CHECK(facility.in_flight());
  CHECK_FALSE(facility.ready(at_ms(49)));
  CHECK(facility.poll(at_ms(49)).error_code() == dfr::error::retransmit_timed_out);

  CHECK(facility.ready(at_ms(50)));
  venue::snapshot_reply reply;
  REQUIRE(facility.poll(at_ms(50)).get(reply) == dfr::error::ok);
  CHECK(reply.session == 0xFEED);
  CHECK(reply.next_sequence == 1'000);
  CHECK_FALSE(facility.in_flight());
  CHECK(facility.stats().replies == 1);
}

TEST_CASE("the position is captured on request, not on reply",
          "[venue][snapshot][regression]") {
  // The single most important line in the facility. Glimpse builds the book and then transmits
  // it, so the state a client receives reflects the feed as it was when the request arrived,
  // and everything published in between is the client's problem. A facility that captured at
  // reply time would always hand back something at least as new as the client's buffer, so the
  // race could never happen and plan_snapshot's behind_buffer verdict would remain a branch no
  // whole client had been driven through.
  test_facility facility{readable_options()};
  facility.advance_to(1'000);
  REQUIRE(facility.request(at_ms(0)).has_value());

  facility.advance_to(9'999);  // the feed runs on while the snapshot is built

  venue::snapshot_reply reply;
  REQUIRE(facility.poll(at_ms(50)).get(reply) == dfr::error::ok);
  CHECK(reply.next_sequence == 1'000);  // not 9,999
}

TEST_CASE("polling an idle facility says so", "[venue][snapshot]") {
  // Rather than returning a plausible empty reply, which a caller would apply.
  test_facility facility{readable_options()};
  CHECK(facility.poll(at_ms(0)).error_code() == dfr::error::not_supported);
}

TEST_CASE("a second request while one is outstanding is refused",
          "[venue][snapshot]") {
  // Glimpse runs over one SoupBinTCP session. A client that fired two and applied whichever
  // returned first would be choosing its own state at random.
  test_facility facility{readable_options()};
  facility.advance_to(500);
  REQUIRE(facility.request(at_ms(0)).has_value());

  const auto refused = facility.request(at_ms(1));
  CHECK(refused.error_code() == dfr::error::not_supported);
  CHECK(facility.stats().refused_in_flight == 1);
}

TEST_CASE("a facility can be asked again after replying",
          "[venue][snapshot]") {
  test_facility facility{readable_options()};
  facility.advance_to(500);
  REQUIRE(facility.request(at_ms(0)).has_value());
  venue::snapshot_reply reply;
  REQUIRE(facility.poll(at_ms(50)).get(reply) == dfr::error::ok);

  facility.advance_to(900);
  REQUIRE(facility.request(at_ms(60)).has_value());
  REQUIRE(facility.poll(at_ms(110)).get(reply) == dfr::error::ok);
  CHECK(reply.next_sequence == 900);
  CHECK(facility.stats().replies == 2);
}

TEST_CASE("the feed position only moves forward", "[venue][snapshot]") {
  // A late or duplicated notification from the publisher must not rewind what the facility
  // believes it can snapshot.
  test_facility facility{readable_options()};
  facility.advance_to(1'000);
  facility.advance_to(500);
  CHECK(facility.feed_position() == 1'000);
}

// ---------------------------------------------------------------------------
// Losing the race deliberately
// ---------------------------------------------------------------------------

TEST_CASE("staleness pushes the snapshot behind the request position",
          "[venue][snapshot]") {
  // The knob that turns the Glimpse race from something that happens under load into something
  // a test can produce on demand.
  auto options = readable_options();
  options.staleness_messages = 400;
  test_facility facility{options};
  facility.advance_to(1'000);

  REQUIRE(facility.request(at_ms(0)).has_value());
  venue::snapshot_reply reply;
  REQUIRE(facility.poll(at_ms(50)).get(reply) == dfr::error::ok);
  CHECK(reply.next_sequence == 600);
}

TEST_CASE("staleness past the start of the session saturates",
          "[venue][snapshot]") {
  // Wrapping would report a snapshot near 2^64: a position no client could interpret, and one
  // that would look like a snapshot far *ahead* of the feed rather than behind it.
  auto options = readable_options();
  options.staleness_messages = 1'000'000;
  test_facility facility{options};
  facility.advance_to(100);

  REQUIRE(facility.request(at_ms(0)).has_value());
  venue::snapshot_reply reply;
  REQUIRE(facility.poll(at_ms(50)).get(reply) == dfr::error::ok);
  CHECK(reply.next_sequence == 1);
}

TEST_CASE("zero latency still requires a poll", "[venue][snapshot]") {
  // A facility that answered inside request() would hide the window in which the race is lost,
  // which is the one thing this component exists to expose.
  auto options = readable_options();
  options.latency = dfr::duration::zero();
  test_facility facility{options};
  facility.advance_to(700);

  REQUIRE(facility.request(at_ms(0)).has_value());
  CHECK(facility.in_flight());
  venue::snapshot_reply reply;
  REQUIRE(facility.poll(at_ms(0)).get(reply) == dfr::error::ok);
  CHECK(reply.next_sequence == 700);
}

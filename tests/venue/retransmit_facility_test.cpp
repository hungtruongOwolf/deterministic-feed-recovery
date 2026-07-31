// The retransmit facility: serving from a window that really forgets.
//
// The reason to build this rather than let a test harness always have the answer: a client
// tested only against a facility that never says no is a client whose window-exceeded path has
// never run, and that path ends in a snapshot.

#include <dfr/venue/retransmit_facility.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rec = dfr::recovery;
namespace venue = dfr::venue;

namespace {

// Small on purpose, so eviction happens in a test rather than in theory.
using small_facility = venue::retransmit_facility<8, 256>;

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

// A packet just long enough to be distinguishable; the facility never looks inside.
std::string body_for(std::uint64_t sequence) {
  return "packet-" + std::to_string(sequence);
}

void must_record(small_facility& facility, std::uint64_t first,
                 std::uint64_t count) {
  const std::string body = body_for(first);
  REQUIRE(facility
              .record(first, count,
                      dfr::packet_view{body.data(), body.size()})
              .has_value());
}

struct sink {
  std::vector<std::string> packets;
  void operator()(dfr::packet_view packet) {
    packets.emplace_back(reinterpret_cast<const char*>(packet.data()),
                         packet.size());
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

TEST_CASE("a fresh facility has nothing to serve", "[venue][facility]") {
  small_facility facility;
  CHECK(facility.retained() == 0);
  CHECK(facility.available().empty());

  sink served;
  rec::sequence_range answered;
  REQUIRE(facility.serve(range(1, 5), served).get(answered) == dfr::error::ok);
  CHECK(answered.empty());
  CHECK(served.packets.empty());
  CHECK(facility.stats().requests_ahead_of_feed == 1);
}

TEST_CASE("the window reports what it can still answer for",
          "[venue][facility]") {
  small_facility facility;
  must_record(facility, 1, 3);
  must_record(facility, 4, 2);

  CHECK(facility.retained() == 2);
  CHECK(facility.available() == range(1, 6));
}

TEST_CASE("heartbeats are not retained", "[venue][facility]") {
  // They carry no messages, so no request could ever be answered with one, and keeping them
  // would let a quiet period push real data out of the window.
  small_facility facility;
  must_record(facility, 1, 3);
  for (int i = 0; i < 20; ++i) {
    must_record(facility, 4, 0);  // heartbeat: count zero
  }

  CHECK(facility.retained() == 1);
  CHECK(facility.available() == range(1, 4));
  CHECK(facility.evicted() == 0);
}

TEST_CASE("the oldest packets fall out of a full window",
          "[venue][facility]") {
  // A ring here, unlike recovery::replay_buffer, and that is not a compromise: forgetting the
  // oldest is the definition of a retention window, while a replay buffer that forgot would
  // make the Glimpse race undetectable.
  small_facility facility;
  for (std::uint64_t i = 0; i < 12; ++i) {
    must_record(facility, 1 + i * 2, 2);
  }

  CHECK(facility.retained() == small_facility::window());
  CHECK(facility.evicted() == 4);
  CHECK(facility.available().first > 1);  // the beginning is gone
}

// ---------------------------------------------------------------------------
// Serving
// ---------------------------------------------------------------------------

TEST_CASE("a request inside the window is served", "[venue][facility]") {
  small_facility facility;
  for (std::uint64_t i = 0; i < 5; ++i) {
    must_record(facility, 1 + i * 2, 2);
  }

  sink served;
  rec::sequence_range answered;
  REQUIRE(facility.serve(range(3, 7), served).get(answered) == dfr::error::ok);
  CHECK(answered == range(3, 7));
  CHECK(served.packets.size() == 2);
  CHECK(facility.stats().messages_served == 4);
}

TEST_CASE("a request is served in publication order",
          "[venue][facility]") {
  // So the client receives the repair the way it would have received the original, and the
  // served range comes out contiguous without a sort.
  small_facility facility;
  for (std::uint64_t i = 0; i < 6; ++i) {
    must_record(facility, 1 + i, 1);
  }

  sink served;
  rec::sequence_range answered;
  REQUIRE(facility.serve(range(2, 6), served).get(answered) == dfr::error::ok);
  REQUIRE(served.packets.size() == 4);
  for (std::size_t i = 0; i < served.packets.size(); ++i) {
    CHECK(served.packets[i] == body_for(2 + i));
  }
  CHECK(answered == range(2, 6));
}

TEST_CASE("a request is served in order after the ring has wrapped",
          "[venue][facility][regression]") {
  // The index has to walk from the oldest retained slot rather than from slot zero. Walking
  // from zero would hand the client the newest packets first, and a receiver applying them in
  // that order would build a wrong book from a correct retransmit.
  small_facility facility;
  for (std::uint64_t i = 0; i < 20; ++i) {
    must_record(facility, 1 + i, 1);
  }
  REQUIRE(facility.evicted() == 12);

  const auto have = facility.available();
  sink served;
  rec::sequence_range answered;
  REQUIRE(facility.serve(have, served).get(answered) == dfr::error::ok);
  REQUIRE(served.packets.size() == small_facility::window());
  for (std::size_t i = 0; i < served.packets.size(); ++i) {
    CHECK(served.packets[i] == body_for(have.first + i));
  }
}

TEST_CASE("a partial overlap serves the packets that cover it",
          "[venue][facility]") {
  // A request rarely lines up with packet boundaries, and a facility that only answered exact
  // matches would answer almost nothing.
  small_facility facility;
  must_record(facility, 1, 5);
  must_record(facility, 6, 5);

  sink served;
  rec::sequence_range answered;
  REQUIRE(facility.serve(range(4, 8), served).get(answered) == dfr::error::ok);
  CHECK(served.packets.size() == 2);
  CHECK(answered == range(1, 11));  // whole packets, which is all the wire can carry
}

// ---------------------------------------------------------------------------
// Saying no
// ---------------------------------------------------------------------------

TEST_CASE("a request reaching before the window is refused whole",
          "[venue][facility]") {
  // Refused rather than partially served. A client handed only the tail would close part of its
  // hole and go on asking for the rest forever, never learning that a snapshot is the only
  // repair left.
  small_facility facility;
  for (std::uint64_t i = 0; i < 20; ++i) {
    must_record(facility, 1 + i, 1);
  }
  const auto have = facility.available();

  sink served;
  const auto refused = facility.serve(range(1, have.first + 2), served);
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::retransmit_window_exceeded);
  CHECK(dfr::is_fatal(refused.error_code()));
  CHECK(served.packets.empty());
  CHECK(facility.stats().refused_too_old == 1);
}

TEST_CASE("a request larger than one response is refused",
          "[venue][facility]") {
  // MoldUDP64's cap. A real server answers a larger request with silence; this reports it,
  // because silence is indistinguishable from a bug in the test.
  small_facility facility;
  must_record(facility, 1, 5);

  sink served;
  const auto refused =
      facility.serve(range(1, 1 + venue::kMaxMessagesPerResponse + 1), served);
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
  CHECK(facility.stats().refused_too_large == 1);
  CHECK(served.packets.empty());
}

TEST_CASE("a request ahead of the feed is answered with nothing",
          "[venue][facility]") {
  // Not an error: the client is early rather than late, and counted separately because that
  // means it is confused rather than merely behind.
  small_facility facility;
  must_record(facility, 1, 5);

  sink served;
  rec::sequence_range answered;
  REQUIRE(facility.serve(range(500, 505), served).get(answered) ==
          dfr::error::ok);
  CHECK(answered.empty());
  CHECK(served.packets.empty());
  CHECK(facility.stats().requests_ahead_of_feed == 1);
}

TEST_CASE("an empty request is answered with nothing",
          "[venue][facility]") {
  small_facility facility;
  must_record(facility, 1, 5);

  sink served;
  rec::sequence_range answered;
  REQUIRE(facility.serve(range(3, 3), served).get(answered) == dfr::error::ok);
  CHECK(answered.empty());
  CHECK(served.packets.empty());
}

TEST_CASE("a packet too large for the window is refused",
          "[venue][facility]") {
  small_facility facility;
  const std::string enormous(1'000, 'x');
  const auto refused = facility.record(
      1, 1, dfr::packet_view{enormous.data(), enormous.size()});
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
  CHECK(facility.retained() == 0);
}

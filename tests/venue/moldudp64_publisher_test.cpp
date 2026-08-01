// The MoldUDP64 publisher, and what one publisher over two protocols has to get right.
//
// The policy in publisher_target.hpp was written when this file arrived, not before it. So the thing worth
// checking is not that the abstraction exists but that it kept the two protocols' differences: MoldUDP64
// has a ten-byte text session and *no* stream offset, and a publisher that maintained one anyway would be
// computing a number with nowhere to put it.

#include <dfr/venue/publisher.hpp>

#include <dfr/wire/moldudp64/cursor.hpp>
#include <dfr/wire/moldudp64/header.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mold = dfr::wire::moldudp64;
namespace venue = dfr::venue;

namespace {

using test_publisher = venue::moldudp64_publisher<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

test_time at_ms(std::int64_t millis) {
  return test_time{} + std::chrono::milliseconds{millis};
}

venue::publisher_options readable_options() {
  venue::publisher_options options;
  options.session = 42;
  options.first_sequence = 1;
  options.heartbeat_interval = std::chrono::milliseconds{100};
  REQUIRE(options.validate().has_value());
  return options;
}

dfr::packet_view bytes_of(std::string_view text) {
  return dfr::packet_view{text.data(), text.size()};
}

struct sink {
  std::vector<std::string> packets;
  void operator()(dfr::packet_view packet) {
    packets.emplace_back(reinterpret_cast<const char*>(packet.data()), packet.size());
  }
  [[nodiscard]] dfr::packet_view at(std::size_t i) const {
    return dfr::packet_view{packets[i].data(), packets[i].size()};
  }
};

// Feeds every emitted packet back through the MoldUDP64 decoder and checks the one chain this protocol
// has: sequence numbers counting messages, not packets.
struct verdict {
  std::uint64_t packets{0};
  std::uint64_t messages{0};
  std::uint64_t heartbeats{0};
  dfr::error first_failure{dfr::error::ok};
  bool chain_intact{true};
};

verdict decode_all(const sink& emitted) {
  verdict out;
  std::uint64_t expected = 0;
  bool started = false;

  for (std::size_t i = 0; i < emitted.packets.size(); ++i) {
    const auto packet = emitted.at(i);
    mold::header header;
    if (const auto err = mold::decode_header(packet).get(header);
        err != dfr::error::ok) {
      out.first_failure = err;
      return out;
    }
    // The block walk must account for exactly the bytes present, which is the check that catches a count
    // disagreeing with its contents: the defect penberg/helix has and dfr::chaos injects on purpose.
    mold::message_cursor cursor;
    if (const auto err = mold::message_cursor::over(packet).get(cursor);
        err != dfr::error::ok) {
      out.first_failure = err;
      return out;
    }
    if (const auto err = cursor.drain([](const mold::message&) {}); !err) {
      out.first_failure = err.error_code();
      return out;
    }

    if (!started) {
      started = true;
      expected = header.sequence;
    }
    if (header.sequence != expected) {
      out.chain_intact = false;
    }
    expected = header.next_sequence();

    ++out.packets;
    out.messages += header.message_count == mold::kHeartbeat ? 0 : header.message_count;
    if (header.kind() == mold::packet_kind::heartbeat) {
      ++out.heartbeats;
    }
  }
  return out;
}

}  // namespace

TEST_CASE("a published MoldUDP64 stream chains by message count",
          "[venue][moldudp64]") {
  // The one chain this protocol has. MoldUDP64's sequence counts *messages*, so a publisher that advanced
  // it per packet would produce a stream whose numbering is short by the packing factor, and a receiver
  // would report a gap on every packet after the first.
  test_publisher publisher{readable_options()};
  sink emitted;

  for (int i = 0; i < 200; ++i) {
    REQUIRE(publisher
                .submit(bytes_of("message-" + std::to_string(i)), at_ms(i), emitted)
                .has_value());
  }
  REQUIRE(publisher.flush(at_ms(500), emitted).has_value());

  const auto checked = decode_all(emitted);
  CHECK(checked.first_failure == dfr::error::ok);
  CHECK(checked.chain_intact);
  CHECK(checked.messages == 200);
  CHECK(checked.packets < 200);  // packed, as a venue packs
}

TEST_CASE("the stream survives heartbeats interleaved",
          "[venue][moldudp64]") {
  // A MoldUDP64 heartbeat carries the *next* sequence and a count of zero, so it must not advance the
  // numbering. If it did, the next data packet would be reported as a gap.
  test_publisher publisher{readable_options()};
  sink emitted;
  std::int64_t now = 0;

  for (int round = 0; round < 20; ++round) {
    REQUIRE(publisher.submit(bytes_of("m"), at_ms(now), emitted).has_value());
    REQUIRE(publisher.flush(at_ms(now), emitted).has_value());
    now += 250;
    REQUIRE(publisher.poll(at_ms(now), emitted).has_value());
  }

  const auto checked = decode_all(emitted);
  CHECK(checked.first_failure == dfr::error::ok);
  CHECK(checked.chain_intact);
  CHECK(checked.messages == 20);
  CHECK(checked.heartbeats == 20);
}

TEST_CASE("MoldUDP64 maintains no stream offset",
          "[venue][moldudp64][regression]") {
  // The substance of the difference between the two protocols. IEX-TP carries a byte position beside the
  // sequence(its second redundant chain) and MoldUDP64 has no field for one. A shared publisher that
  // maintained it for both would be computing a number with nowhere to put it, and the flag that prevents
  // that is checked here rather than trusted.
  STATIC_REQUIRE(!venue::moldudp64_target::kTracksStreamOffset);
  STATIC_REQUIRE(venue::iextp_target::kTracksStreamOffset);

  test_publisher publisher{readable_options()};
  sink emitted;
  for (int i = 0; i < 10; ++i) {
    REQUIRE(publisher.submit(bytes_of("payload"), at_ms(i), emitted).has_value());
  }
  REQUIRE(publisher.flush(at_ms(10), emitted).has_value());

  // The offset never moves, because nothing reads it.
  CHECK(publisher.next_stream_offset() == 0);
  CHECK(publisher.next_sequence() == 11);
}

TEST_CASE("the session is ten bytes of text, not an integer",
          "[venue][moldudp64]") {
  // MoldUDP64's session field is alphanumeric. The configured numeric session is written into the last four
  // bytes so the field stays printable and two configured sessions stay distinguishable: the same
  // convention chaos::moldudp64_target uses when it rewrites the field, so a fault injected into a
  // published stream lands where a reader expects it.
  test_publisher publisher{readable_options()};
  sink emitted;
  REQUIRE(publisher.submit(bytes_of("one"), at_ms(0), emitted).has_value());
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());

  mold::header header;
  REQUIRE(mold::decode_header(emitted.at(0)).get(header) == dfr::error::ok);
  CHECK(header.session.text() == "      0042");
  CHECK(header.sequence == 1);
  CHECK(header.message_count == 1);
}

TEST_CASE("a feed can start anywhere", "[venue][moldudp64]") {
  auto options = readable_options();
  options.first_sequence = 5'000'000;
  test_publisher publisher{options};
  sink emitted;

  REQUIRE(publisher.submit(bytes_of("first"), at_ms(0), emitted).has_value());
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());

  mold::header header;
  REQUIRE(mold::decode_header(emitted.at(0)).get(header) == dfr::error::ok);
  CHECK(header.sequence == 5'000'000);
  CHECK(decode_all(emitted).chain_intact);
}

TEST_CASE("one publisher drives both protocols from the same driver",
          "[venue][moldudp64]") {
  // The claim the policy exists to support. The same sequence of calls produces a valid stream in each
  // protocol, and the only visible difference is the one the protocols actually have.
  venue::moldudp64_publisher<dfr::manual_clock> mold_side{readable_options()};
  venue::iextp_publisher<dfr::manual_clock> iex_side{readable_options()};
  sink mold_out;
  sink iex_out;

  for (int i = 0; i < 30; ++i) {
    const std::string body = "msg-" + std::to_string(i);
    REQUIRE(mold_side.submit(bytes_of(body), at_ms(i), mold_out).has_value());
    REQUIRE(iex_side.submit(bytes_of(body), at_ms(i), iex_out).has_value());
  }
  REQUIRE(mold_side.flush(at_ms(50), mold_out).has_value());
  REQUIRE(iex_side.flush(at_ms(50), iex_out).has_value());

  CHECK(mold_side.next_sequence() == iex_side.next_sequence());
  CHECK(mold_side.next_stream_offset() == 0);
  CHECK(iex_side.next_stream_offset() > 0);
  CHECK(decode_all(mold_out).chain_intact);
}

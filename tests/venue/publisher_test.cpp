// The publisher, checked against the decoder that was verified on real data.
//
// This is the point of having a publisher rather than a test helper: every packet it produces
// is fed straight back through wire::iextp, and chain_checker, which held on 460,578 real IEX
// packets: has to accept the result. The encoder and the decoder check each other, and one of
// them has already been checked against reality.

#include <dfr/venue/publisher.hpp>

#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/cursor.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iex = dfr::wire::iextp;
namespace venue = dfr::venue;

namespace {

using test_publisher = venue::iextp_publisher<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

test_time at_ms(std::int64_t millis) {
  return test_time{} + std::chrono::milliseconds{millis};
}

venue::publisher_options readable_options() {
  venue::publisher_options options;
  options.session = 0xFEED;
  options.channel = 1;
  options.first_sequence = 1;
  options.heartbeat_interval = std::chrono::milliseconds{100};
  REQUIRE(options.validate().has_value());
  return options;
}

dfr::packet_view bytes_of(std::string_view text) {
  return dfr::packet_view{text.data(), text.size()};
}

// Collects emitted packets as owned copies, since the publisher reuses one buffer.
struct sink {
  std::vector<std::string> packets;

  void operator()(dfr::packet_view packet) {
    packets.emplace_back(reinterpret_cast<const char*>(packet.data()),
                         packet.size());
  }

  [[nodiscard]] dfr::packet_view at(std::size_t i) const {
    return dfr::packet_view{packets[i].data(), packets[i].size()};
  }
};

// Feeds every emitted packet through the decoder and both cross-packet chains.
struct verdict {
  std::uint64_t packets{0};
  std::uint64_t messages{0};
  std::uint64_t heartbeats{0};
  dfr::error first_failure{dfr::error::ok};
};

verdict decode_all(const sink& emitted) {
  verdict out;
  iex::chain_checker checker;
  for (std::size_t i = 0; i < emitted.packets.size(); ++i) {
    const auto packet = emitted.at(i);
    iex::header header;
    if (const auto err = iex::decode_header(packet).get(header);
        err != dfr::error::ok) {
      out.first_failure = err;
      return out;
    }
    if (const auto framing = iex::verify_payload_framing(packet); !framing) {
      out.first_failure = framing.error_code();
      return out;
    }
    if (const auto chained = checker.observe(header); !chained) {
      out.first_failure = chained.error_code();
      return out;
    }
    ++out.packets;
    out.messages += header.message_count;
    if (header.kind() == iex::packet_kind::heartbeat) {
      ++out.heartbeats;
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

TEST_CASE("the default publisher options are legal", "[venue][publisher]") {
  CHECK(venue::publisher_options{}.validate().has_value());
}

TEST_CASE("a non-positive heartbeat interval is rejected",
          "[venue][publisher]") {
  // Zero would emit a heartbeat on every poll, so a quiet feed would be busier than an active
  // one and the heartbeat would stop carrying information.
  venue::publisher_options options;
  options.heartbeat_interval = dfr::duration::zero();
  CHECK(options.validate().error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// The chains
// ---------------------------------------------------------------------------

TEST_CASE("a published stream satisfies both chains", "[venue][publisher]") {
  // The assertion the whole component exists for. Sequence numbers count messages and Stream
  // Offset counts payload bytes; getting either wrong produces a stream the verified decoder
  // rejects.
  test_publisher publisher{readable_options()};
  sink emitted;

  for (int i = 0; i < 200; ++i) {
    REQUIRE(publisher
                .submit(bytes_of("message-" + std::to_string(i)), at_ms(i),
                        emitted)
                .has_value());
  }
  REQUIRE(publisher.flush(at_ms(500), emitted).has_value());

  const auto checked = decode_all(emitted);
  CHECK(checked.first_failure == dfr::error::ok);
  CHECK(checked.messages == 200);
  CHECK(checked.packets == emitted.packets.size());
}

TEST_CASE("the stream is intact with heartbeats interleaved",
          "[venue][publisher]") {
  // A heartbeat must advance neither chain. If it advanced the offset, the next data packet
  // would fail the offset check while the sequence check passed: the exact class of fault
  // IEX-TP's redundancy exists to catch, so a publisher that produced it would be indicting
  // itself.
  test_publisher publisher{readable_options()};
  sink emitted;
  std::int64_t now = 0;

  for (int round = 0; round < 20; ++round) {
    REQUIRE(publisher.submit(bytes_of("m"), at_ms(now), emitted).has_value());
    REQUIRE(publisher.flush(at_ms(now), emitted).has_value());
    now += 250;  // longer than the interval, so a heartbeat is due
    REQUIRE(publisher.poll(at_ms(now), emitted).has_value());
  }

  const auto checked = decode_all(emitted);
  CHECK(checked.first_failure == dfr::error::ok);
  CHECK(checked.messages == 20);
  CHECK(checked.heartbeats == 20);
  CHECK(publisher.stats().heartbeats == 20);
}

TEST_CASE("a feed can start anywhere, not only at sequence one",
          "[venue][publisher]") {
  // A receiver joining mid-session is the normal case, and a publisher that always started at
  // one would let a client get away with assuming it.
  auto options = readable_options();
  options.first_sequence = 8'000'000;
  options.first_stream_offset = 123'456;
  test_publisher publisher{options};
  sink emitted;

  REQUIRE(publisher.submit(bytes_of("first"), at_ms(0), emitted).has_value());
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());

  iex::header header;
  REQUIRE(iex::decode_header(emitted.at(0)).get(header) == dfr::error::ok);
  CHECK(header.first_sequence == 8'000'000);
  CHECK(header.stream_offset == 123'456);
  CHECK(decode_all(emitted).first_failure == dfr::error::ok);
}

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

TEST_CASE("messages are packed, not sent one per packet",
          "[venue][publisher]") {
  // What a venue actually does, and the reason it matters: a receiver tested only against
  // one-message packets never exercises the difference between a lost packet and a lost
  // message, which is what the whole of dfr::recovery is built on.
  test_publisher publisher{readable_options()};
  sink emitted;

  for (int i = 0; i < 100; ++i) {
    REQUIRE(publisher.submit(bytes_of("abcdefgh"), at_ms(0), emitted).has_value());
  }
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());

  CHECK(emitted.packets.size() < 100);
  const auto checked = decode_all(emitted);
  CHECK(checked.first_failure == dfr::error::ok);
  CHECK(checked.messages == 100);
}

TEST_CASE("a full datagram is flushed and a new one started",
          "[venue][publisher]") {
  // And no message is split across two packets, which IEX-TP has no way to express.
  test_publisher publisher{readable_options()};
  sink emitted;
  const std::string body(400, 'x');

  for (int i = 0; i < 12; ++i) {
    REQUIRE(publisher.submit(bytes_of(body), at_ms(0), emitted).has_value());
  }
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());

  REQUIRE(emitted.packets.size() > 1);
  for (const auto& packet : emitted.packets) {
    CHECK(packet.size() <= venue::kMaxDatagramBytes);
  }
  const auto checked = decode_all(emitted);
  CHECK(checked.first_failure == dfr::error::ok);
  CHECK(checked.messages == 12);
}

TEST_CASE("a message too large for any datagram is refused",
          "[venue][publisher]") {
  // Reported rather than asserted: the message came from outside the library.
  test_publisher publisher{readable_options()};
  sink emitted;
  const std::string enormous(2'000, 'x');

  const auto refused = publisher.submit(bytes_of(enormous), at_ms(0), emitted);
  CHECK(refused.error_code() == dfr::error::invalid_argument);
  CHECK(emitted.packets.empty());
}

// ---------------------------------------------------------------------------
// Heartbeats
// ---------------------------------------------------------------------------

TEST_CASE("a heartbeat carries the next sequence and advances nothing",
          "[venue][publisher]") {
  test_publisher publisher{readable_options()};
  sink emitted;
  REQUIRE(publisher.submit(bytes_of("one"), at_ms(0), emitted).has_value());
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());

  const auto sequence_before = publisher.next_sequence();
  const auto offset_before = publisher.next_stream_offset();
  REQUIRE(publisher.send_heartbeat(at_ms(1), emitted).has_value());

  iex::header beat;
  REQUIRE(iex::decode_header(emitted.at(1)).get(beat) == dfr::error::ok);
  CHECK(beat.kind() == iex::packet_kind::heartbeat);
  CHECK(beat.message_count == 0);
  CHECK(beat.payload_length == 0);
  CHECK(beat.first_sequence == sequence_before);
  CHECK(beat.stream_offset == offset_before);
  CHECK(publisher.next_sequence() == sequence_before);
  CHECK(publisher.next_stream_offset() == offset_before);
}

TEST_CASE("polling a quiet feed heartbeats only when due",
          "[venue][publisher]") {
  test_publisher publisher{readable_options()};
  sink emitted;
  REQUIRE(publisher.submit(bytes_of("one"), at_ms(0), emitted).has_value());
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());
  REQUIRE(emitted.packets.size() == 1);

  REQUIRE(publisher.poll(at_ms(50), emitted).has_value());
  CHECK(emitted.packets.size() == 1);  // not yet
  REQUIRE(publisher.poll(at_ms(100), emitted).has_value());
  CHECK(emitted.packets.size() == 2);
}

TEST_CASE("polling with messages pending sends them instead of a heartbeat",
          "[venue][publisher]") {
  // Otherwise a heartbeat would overtake data the publisher already had, announcing a sequence
  // it had not sent.
  test_publisher publisher{readable_options()};
  sink emitted;
  REQUIRE(publisher.submit(bytes_of("waiting"), at_ms(0), emitted).has_value());
  REQUIRE(publisher.has_pending());

  REQUIRE(publisher.poll(at_ms(500), emitted).has_value());
  REQUIRE(emitted.packets.size() == 1);
  iex::header header;
  REQUIRE(iex::decode_header(emitted.at(0)).get(header) == dfr::error::ok);
  CHECK(header.message_count == 1);
  CHECK(publisher.stats().heartbeats == 0);
}

TEST_CASE("flushing an empty publisher emits nothing", "[venue][publisher]") {
  // An empty data packet and a heartbeat are the same bytes on the wire, so conflating them
  // would make the heartbeat count meaningless.
  test_publisher publisher{readable_options()};
  sink emitted;
  REQUIRE(publisher.flush(at_ms(0), emitted).has_value());
  CHECK(emitted.packets.empty());
  CHECK(publisher.stats().packets == 0);
}

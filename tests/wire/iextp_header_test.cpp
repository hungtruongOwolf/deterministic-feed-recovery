#include <dfr/wire/iextp.hpp>

#include <dfr/core/mutable_packet_view.hpp>

#include "support/death_test.hpp"
#include "wire/support/raw_iextp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace iex = dfr::wire::iextp;
using dfr_test::iex::as_text;
using dfr_test::iex::prototype;
using dfr_test::iex::raw_packet;

// ---------------------------------------------------------------------------
// Wire constants
// ---------------------------------------------------------------------------

TEST_CASE("the header layout matches the specification", "[wire][iextp]") {
  STATIC_REQUIRE(iex::kHeaderSize == 40);
  STATIC_REQUIRE(iex::kVersionOffset == 0);
  STATIC_REQUIRE(iex::kProtocolIdOffset == 2);
  STATIC_REQUIRE(iex::kChannelIdOffset == 4);
  STATIC_REQUIRE(iex::kSessionIdOffset == 8);
  STATIC_REQUIRE(iex::kPayloadLengthOffset == 12);
  STATIC_REQUIRE(iex::kMessageCountOffset == 14);
  STATIC_REQUIRE(iex::kStreamOffsetOffset == 16);
  STATIC_REQUIRE(iex::kFirstSequenceOffset == 24);
  STATIC_REQUIRE(iex::kSendTimeOffset == 32);
}

TEST_CASE("IEX-TP is little-endian where MoldUDP64 is big-endian",
          "[wire][iextp]") {
  // The contrast that justifies putting byte order in every accessor's name.
  // Sequence 1 little-endian puts 0x01 in the *first* byte of the field; the
  // same value big-endian would put it in the last.
  const auto packet = raw_packet{}.first_sequence(1).seal();

  iex::header decoded;
  REQUIRE(iex::decode_header(packet.view()).get(decoded) == dfr::error::ok);
  CHECK(decoded.first_sequence == 1);

  const dfr::packet_view raw = packet.view();
  CHECK(raw.u8_at(iex::kFirstSequenceOffset) == 0x01);
  CHECK(raw.u8_at(iex::kFirstSequenceOffset + 7) == 0x00);

  // And reading the same bytes big-endian gives a wildly different value, which
  // is what would silently happen if the accessor did not name its order.
  CHECK(raw.be64_at(iex::kFirstSequenceOffset) != 1);
}

// ---------------------------------------------------------------------------
// decode_header
// ---------------------------------------------------------------------------

TEST_CASE("a header decodes every field", "[wire][iextp]") {
  const auto packet = raw_packet{}
                          .protocol(0x8004)
                          .channel(7)
                          .session(0xDEADBEEF)
                          .count(3)
                          .stream_offset(4096)
                          .first_sequence(1000)
                          .send_time(123'456'789)
                          .block("aaa")
                          .block("bb")
                          .block("c")
                          .seal();

  iex::header decoded;
  REQUIRE(iex::decode_header(packet.view()).get(decoded) == dfr::error::ok);

  CHECK(decoded.version == iex::kVersion);
  CHECK(decoded.protocol == 0x8004);
  CHECK(decoded.channel == 7);
  CHECK(decoded.session == 0xDEADBEEF);
  CHECK(decoded.message_count == 3);
  CHECK(decoded.stream_offset == 4096);
  CHECK(decoded.first_sequence == 1000);
  CHECK(decoded.send_time_ns == 123'456'789);
  CHECK(decoded.kind() == iex::packet_kind::data);
}

TEST_CASE("an unknown transport version is rejected", "[wire][iextp]") {
  // not_supported rather than unknown_message_type: the *transport* version is
  // wrong, so every field offset used to read the packet is a guess. Continuing
  // would report confident nonsense.
  const auto packet = raw_packet{}.version(2).seal();
  const auto decoded = iex::decode_header(packet.view());

  CHECK_FALSE(decoded.has_value());
  CHECK(decoded.error_code() == dfr::error::not_supported);
}

TEST_CASE("a datagram shorter than the header is truncated_header",
          "[wire][iextp]") {
  const std::array<std::uint8_t, iex::kHeaderSize - 1> short_packet{};
  const auto decoded = iex::decode_header(
      dfr::packet_view{short_packet.data(), short_packet.size()});

  CHECK_FALSE(decoded.has_value());
  CHECK(decoded.error_code() == dfr::error::truncated_header);
}

TEST_CASE("a stream offset stays signed", "[wire][iextp]") {
  // The specification says signed. Widening to unsigned would turn a publisher
  // fault into a plausible-looking offset near 2^64 instead of an obvious
  // negative one.
  const auto packet = raw_packet{}.stream_offset(-1).seal();

  iex::header decoded;
  REQUIRE(iex::decode_header(packet.view()).get(decoded) == dfr::error::ok);
  CHECK(decoded.stream_offset == -1);
}

// ---------------------------------------------------------------------------
// Heartbeats
// ---------------------------------------------------------------------------

TEST_CASE("a heartbeat needs both count and payload length zero",
          "[wire][iextp]") {
  // Unlike MoldUDP64 there is no sentinel count, so both fields carry the
  // signal. Checking only one is a way to mistake a malformed packet for a
  // heartbeat.
  const iex::header beat{};
  CHECK(beat.kind() == iex::packet_kind::heartbeat);
  CHECK(beat.next_sequence() == beat.first_sequence);

  const iex::header count_only{.payload_length = 5, .message_count = 0};
  CHECK(count_only.kind() == iex::packet_kind::data);

  const iex::header payload_only{.payload_length = 0, .message_count = 2};
  CHECK(payload_only.kind() == iex::packet_kind::data);
}

TEST_CASE("packet kinds and protocol ids have names", "[wire][iextp]") {
  CHECK(iex::to_string(iex::packet_kind::data) == "data");
  CHECK(iex::to_string(iex::packet_kind::heartbeat) == "heartbeat");
  CHECK(iex::to_string(iex::protocol_id::deep) == "DEEP");
  CHECK(iex::to_string(iex::protocol_id::tops) == "TOPS");
}

// ---------------------------------------------------------------------------

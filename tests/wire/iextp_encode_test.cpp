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
// Encoding
// ---------------------------------------------------------------------------

TEST_CASE("a header round-trips through encode and decode", "[wire][iextp]") {
  const iex::header original{.version = iex::kVersion,
                             .reserved = 0,
                             .protocol = 0x8004,
                             .channel = 0x1234'5678,
                             .session = 0x9ABC'DEF0,
                             .payload_length = 40,
                             .message_count = 5,
                             .stream_offset = -12'345,
                             .first_sequence = 0xFEDC'BA98'7654'3210,
                             .send_time_ns = 0x0011'2233'4455'6677};

  std::array<std::uint8_t, iex::kHeaderSize> buffer{};
  std::size_t written = 0;
  REQUIRE(iex::encode_header(
              dfr::mutable_packet_view{buffer.data(), buffer.size()}, original)
              .get(written) == dfr::error::ok);
  CHECK(written == iex::kHeaderSize);

  iex::header decoded;
  REQUIRE(iex::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(decoded) == dfr::error::ok);
  CHECK(decoded == original);
}

TEST_CASE("the builder computes both redundant fields", "[wire][iextp]") {
  // It matters more here than for MoldUDP64: this encoder maintains two fields
  // that are redundant with the content, so letting a caller supply either one
  // would allow a packet that fails its own chain check.
  std::array<std::uint8_t, 256> buffer{};
  iex::packet_builder builder{
      *iex::packet_builder::into(
          dfr::mutable_packet_view{buffer.data(), buffer.size()}, prototype())};

  const std::array<std::string_view, 3> bodies{"hello", "x", "worlds!"};
  for (const std::string_view body : bodies) {
    REQUIRE(builder.append(dfr::packet_view{body.data(), body.size()})
                .has_value());
  }

  dfr::packet_view finished;
  REQUIRE(builder.finish().get(finished) == dfr::error::ok);

  iex::header decoded;
  REQUIRE(iex::decode_header(finished).get(decoded) == dfr::error::ok);
  CHECK(decoded.message_count == 3);
  // Two bytes of length per block plus the payloads.
  CHECK(decoded.payload_length == 3 * 2 + 5 + 1 + 7);
  CHECK(decoded.first_sequence == 1);
  CHECK(decoded.next_sequence() == 4);

  // And the packet it produced passes every chain it declares.
  CHECK(iex::verify_payload_framing(finished).has_value());

  std::vector<std::string_view> seen;
  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(finished).get(cursor) == dfr::error::ok);
  REQUIRE(cursor
              .drain([&](const iex::message& m) {
                seen.push_back(as_text(m.payload));
              })
              .has_value());
  CHECK(seen == std::vector<std::string_view>{"hello", "x", "worlds!"});
}

TEST_CASE("a built packet chains with the next one", "[wire][iextp]") {
  // The acceptance test for the encoder: two packets built in sequence must
  // satisfy the chain checker, which is the same check a real feed must pass.
  std::array<std::uint8_t, 256> first_buffer{};
  std::array<std::uint8_t, 256> second_buffer{};

  iex::header proto = prototype();
  iex::packet_builder first{*iex::packet_builder::into(
      dfr::mutable_packet_view{first_buffer.data(), first_buffer.size()}, proto)};
  REQUIRE(first.append(dfr::packet_view{"abc", 3}).has_value());
  REQUIRE(first.append(dfr::packet_view{"de", 2}).has_value());

  dfr::packet_view first_packet;
  REQUIRE(first.finish().get(first_packet) == dfr::error::ok);

  iex::header first_header;
  REQUIRE(iex::decode_header(first_packet).get(first_header) == dfr::error::ok);

  // Advance the prototype exactly as a publisher would.
  proto.first_sequence = first_header.next_sequence();
  proto.stream_offset = first_header.next_stream_offset();

  iex::packet_builder second{*iex::packet_builder::into(
      dfr::mutable_packet_view{second_buffer.data(), second_buffer.size()},
      proto)};
  REQUIRE(second.append(dfr::packet_view{"fgh", 3}).has_value());

  dfr::packet_view second_packet;
  REQUIRE(second.finish().get(second_packet) == dfr::error::ok);

  iex::header second_header;
  REQUIRE(iex::decode_header(second_packet).get(second_header) ==
          dfr::error::ok);

  iex::chain_checker checker;
  CHECK(checker.observe(first_header).has_value());
  CHECK(checker.observe(second_header).has_value());
}

TEST_CASE("a heartbeat zeroes both signal fields", "[wire][iextp]") {
  std::array<std::uint8_t, iex::kHeaderSize> buffer{};
  iex::header proto = prototype();
  proto.message_count = 9;
  proto.payload_length = 99;

  REQUIRE(iex::encode_heartbeat(
              dfr::mutable_packet_view{buffer.data(), buffer.size()}, proto)
              .has_value());

  iex::header decoded;
  REQUIRE(iex::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(decoded) == dfr::error::ok);
  CHECK(decoded.kind() == iex::packet_kind::heartbeat);
  CHECK(decoded.message_count == 0);
  CHECK(decoded.payload_length == 0);
}

TEST_CASE("the builder refuses a payload it cannot describe",
          "[wire][iextp]") {
  std::array<std::uint8_t, iex::kHeaderSize + 4> buffer{};
  iex::packet_builder builder{*iex::packet_builder::into(
      dfr::mutable_packet_view{buffer.data(), buffer.size()}, prototype())};

  REQUIRE(builder.append(dfr::packet_view{"ab", 2}).has_value());
  const auto rejected = builder.append(dfr::packet_view{"c", 1});
  CHECK_FALSE(rejected.has_value());
  CHECK(rejected.error_code() == dfr::error::capacity_exceeded);

  // The rejection left the packet intact.
  CHECK(builder.message_count() == 1);
  CHECK(builder.finish().has_value());
}

TEST_CASE("a header decodes at compile time", "[wire][iextp]") {
  static constexpr std::array<std::byte, iex::kHeaderSize> bytes{
      std::byte{0x01}, std::byte{0x00},                    // version, reserved
      std::byte{0x04}, std::byte{0x80},                    // protocol LE
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00},                    // payload length 0
      std::byte{0x00}, std::byte{0x00},                    // count 0
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x2A}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  static constexpr dfr::packet_view packet{bytes.data(), bytes.size()};
  constexpr auto decoded = iex::decode_header(packet);

  STATIC_REQUIRE(decoded.has_value());
  STATIC_REQUIRE(decoded.value_unsafe().protocol == 0x8004);
  STATIC_REQUIRE(decoded.value_unsafe().channel == 1);
  STATIC_REQUIRE(decoded.value_unsafe().session == 2);
  STATIC_REQUIRE(decoded.value_unsafe().first_sequence == 42);
  STATIC_REQUIRE(decoded.value_unsafe().kind() == iex::packet_kind::heartbeat);
}

// Framing a SoupBinTCP stream: the length arithmetic, and "not yet" versus "wrong".

#include <dfr/wire/soupbintcp/packet.hpp>

#include "wire/support/raw_soup.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace soup = dfr::wire::soupbintcp;

TEST_CASE("a heartbeat is a length of one and no payload",
          "[wire][soupbintcp]") {
  dfr_test::soup::raw_stream stream;
  stream.frame('H', "");

  soup::packet packet;
  REQUIRE(soup::decode(stream.view()).get(packet) == dfr::error::ok);
  CHECK(packet.type == soup::packet_type::server_heartbeat);
  CHECK(packet.payload.empty());
  CHECK(packet.frame_size == 3);  // two length bytes plus the type
  CHECK_FALSE(packet.advances_sequence());
}

TEST_CASE("the length field counts the type byte and not itself",
          "[wire][soupbintcp][regression]") {
  // The one arithmetic in this protocol that is easy to get wrong. A payload of five bytes gives a
  // declared length of six and a frame of eight. An implementation reading the field as a payload
  // length would report a four-byte payload and a seven-byte frame, and would then start the next
  // packet one byte early, which corrupts everything after it.
  //
  // Heartbeats work under either reading, which is what lets the mistake survive testing.
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "abcde");

  soup::packet packet;
  REQUIRE(soup::decode(stream.view()).get(packet) == dfr::error::ok);
  CHECK(packet.payload.size() == 5);
  CHECK(packet.frame_size == 8);
  CHECK(stream.size() == 8);
}

TEST_CASE("only a sequenced data packet advances the sequence",
          "[wire][soupbintcp]") {
  // A debug packet can arrive between two sequenced ones, and counting it would shift every message
  // after it by one: silently, because nothing on the wire states a position to disagree with.
  CHECK(soup::advances_sequence(soup::packet_type::sequenced_data));
  CHECK_FALSE(soup::advances_sequence(soup::packet_type::debug));
  CHECK_FALSE(soup::advances_sequence(soup::packet_type::unsequenced_data));
  CHECK_FALSE(soup::advances_sequence(soup::packet_type::server_heartbeat));
  CHECK_FALSE(soup::advances_sequence(soup::packet_type::login_accepted));
}

TEST_CASE("a direction is attributable to each packet type",
          "[wire][soupbintcp]") {
  // Used to catch a session wired backwards, which otherwise presents as a stream of packets that
  // decode perfectly and mean nothing.
  CHECK(soup::from_server(soup::packet_type::sequenced_data));
  CHECK(soup::from_server(soup::packet_type::login_accepted));
  CHECK_FALSE(soup::from_server(soup::packet_type::login_request));
  CHECK_FALSE(soup::from_server(soup::packet_type::unsequenced_data));
  CHECK_FALSE(soup::from_server(soup::packet_type::client_heartbeat));
}

// ---------------------------------------------------------------------------
// Not yet, versus wrong
// ---------------------------------------------------------------------------

TEST_CASE("a stream with no length field yet asks for more bytes",
          "[wire][soupbintcp]") {
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "abcde");

  for (std::size_t held = 0; held < 2; ++held) {
    CHECK(soup::decode(stream.prefix(held)).error_code() ==
          dfr::error::need_more_bytes);
  }
}

TEST_CASE("a packet that has not finished arriving asks for more bytes",
          "[wire][soupbintcp][regression]") {
  // The condition TCP has and UDP does not. A reader that reported this as truncated would discard
  // data that was merely still in flight, and on a stream that happens often: a message split
  // across two segments is not an error, it is Tuesday.
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "abcdefghij");

  for (std::size_t held = 2; held < stream.size(); ++held) {
    const auto partial = soup::decode(stream.prefix(held));
    CHECK_FALSE(partial.has_value());
    CHECK(partial.error_code() == dfr::error::need_more_bytes);
  }
  CHECK(soup::decode(stream.view()).has_value());
}

TEST_CASE("asking for more bytes is not fatal", "[wire][soupbintcp]") {
  // Because the stream is fine. Classifying it as fatal would make a caller tear down a healthy
  // connection every time a segment boundary landed mid-packet.
  CHECK_FALSE(dfr::is_fatal(dfr::error::need_more_bytes));
}

TEST_CASE("a declared length of zero is corruption, not an empty packet",
          "[wire][soupbintcp]") {
  // Zero would claim a packet with no type byte, and there is no such thing. Waiting for more bytes
  // here would stall forever on a packet the sender already finished.
  dfr_test::soup::raw_stream stream;
  stream.declared_frame(0, 'H', "");
  CHECK(soup::decode(stream.view()).error_code() == dfr::error::truncated_header);
}

TEST_CASE("an implausible length is refused rather than waited for",
          "[wire][soupbintcp]") {
  // A corrupted length field would otherwise stall the session indefinitely, waiting for bytes the
  // sender never intends to send. There is no framing to resynchronise against in a byte stream, so
  // the only honest answer is to report it and let the caller drop the connection.
  dfr_test::soup::raw_stream stream;
  stream.declared_frame(60'000, 'S', "abc");
  CHECK(soup::decode(stream.view()).error_code() == dfr::error::capacity_exceeded);
}

TEST_CASE("an unknown type frames correctly and is reported as unknown",
          "[wire][soupbintcp]") {
  // Decoding accepts it on purpose: the framing is valid, and a session that dropped the connection
  // over one unrecognised type would be less robust than the specification requires. What to do with
  // it is the caller's decision, so the question is answerable separately.
  dfr_test::soup::raw_stream stream;
  stream.frame('\x7f', "payload");

  soup::packet packet;
  REQUIRE(soup::decode(stream.view()).get(packet) == dfr::error::ok);
  CHECK(packet.payload.size() == 7);
  CHECK_FALSE(soup::is_known(packet.type));
  CHECK(soup::is_known(soup::packet_type::sequenced_data));
}

TEST_CASE("a packet decodes at compile time", "[wire][soupbintcp]") {
  static constexpr std::array<std::byte, 6> kBytes{
      std::byte{0x00}, std::byte{0x04}, std::byte{'S'},
      std::byte{'a'},  std::byte{'b'},  std::byte{'c'}};
  static_assert(soup::decode(dfr::packet_view{kBytes.data(), kBytes.size()})
                    .has_value());
  static_assert(soup::decode(dfr::packet_view{kBytes.data(), kBytes.size()})
                    .value()
                    .frame_size == 6);
  SUCCEED("the static_asserts above are the test");
}

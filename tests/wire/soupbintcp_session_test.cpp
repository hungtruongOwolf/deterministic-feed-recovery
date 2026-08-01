// The login handshake and the sequence the protocol never puts on the wire.

#include <dfr/wire/soupbintcp/cursor.hpp>
#include <dfr/wire/soupbintcp/encode.hpp>
#include <dfr/wire/soupbintcp/login.hpp>

#include "wire/support/raw_soup.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace soup = dfr::wire::soupbintcp;

namespace {

// Walks a whole buffer, collecting what came out. Mirrors how a caller uses the cursor: read what is
// there, keep the remainder, append more later.
struct walked {
  std::vector<soup::sequenced_packet> packets;
  dfr::error stopped{dfr::error::ok};
  std::size_t consumed{0};
};

walked walk(soup::stream_cursor& cursor, dfr::packet_view stream) {
  walked out;
  std::size_t at = 0;
  for (;;) {
    // Stop when the buffer is exhausted rather than asking the cursor about zero bytes. An empty view
    // genuinely needs more bytes, so calling anyway would report a fully consumed buffer as a partial
    // packet: the two are different states and a caller tells them apart by what is left over.
    if (at == stream.size()) {
      break;
    }
    dfr::packet_view rest;
    if (stream.subview(at, stream.size() - at).get(rest) != dfr::error::ok) {
      break;
    }
    soup::sequenced_packet packet;
    const auto err = cursor.next(rest).get(packet);
    if (err != dfr::error::ok) {
      out.stopped = err;
      break;
    }
    out.packets.push_back(packet);
    at += packet.frame.frame_size;
  }
  out.consumed = at;
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

TEST_CASE("login accepted names the next sequence, not the last one",
          "[wire][soupbintcp]") {
  // The off-by-one here shifts every message a client will ever number, and nothing later in the
  // session states a position to disagree with, so it is silent and permanent.
  dfr_test::soup::raw_stream stream;
  stream.login_accepted("SESS01", "12345");

  soup::packet frame;
  REQUIRE(soup::decode(stream.view()).get(frame) == dfr::error::ok);
  REQUIRE(frame.type == soup::packet_type::login_accepted);

  soup::login_accepted accepted;
  REQUIRE(soup::decode_login_accepted(frame.payload).get(accepted) ==
          dfr::error::ok);
  CHECK(accepted.session == "SESS01");
  CHECK(accepted.next_sequence == 12'345);
}

TEST_CASE("login accepted round-trips through the encoder",
          "[wire][soupbintcp]") {
  std::array<std::byte, 64> buffer{};
  const dfr::mutable_packet_view out{buffer.data(), buffer.size()};
  std::size_t written = 0;
  REQUIRE(soup::encode_login_accepted(out, "GLIMPSE1", 987'654'321)
              .get(written) == dfr::error::ok);
  CHECK(written == 3 + 30);

  soup::packet frame;
  REQUIRE(soup::decode(dfr::packet_view{buffer.data(), written}).get(frame) ==
          dfr::error::ok);
  soup::login_accepted accepted;
  REQUIRE(soup::decode_login_accepted(frame.payload).get(accepted) ==
          dfr::error::ok);
  CHECK(accepted.session == "GLIMPSE1");
  CHECK(accepted.next_sequence == 987'654'321);
}

TEST_CASE("a login accepted of the wrong size is reported exactly",
          "[wire][soupbintcp]") {
  // Not "at least thirty bytes". A short field reads as a shorter number and a long one means the
  // framing was misread: both worth reporting rather than interpreting.
  dfr_test::soup::raw_stream stream;
  stream.frame('A', "SESS01    12345");

  soup::packet frame;
  REQUIRE(soup::decode(stream.view()).get(frame) == dfr::error::ok);
  CHECK(soup::decode_login_accepted(frame.payload).error_code() ==
        dfr::error::message_length_mismatch);
}

TEST_CASE("a login request round-trips, including its two conventions",
          "[wire][soupbintcp]") {
  std::array<std::byte, 128> buffer{};
  const dfr::mutable_packet_view out{buffer.data(), buffer.size()};
  std::size_t written = 0;
  REQUIRE(soup::encode_login_request(out, "USER01", "SECRET", "SESS01", 1)
              .get(written) == dfr::error::ok);
  CHECK(written == 3 + 46);

  soup::packet frame;
  REQUIRE(soup::decode(dfr::packet_view{buffer.data(), written}).get(frame) ==
          dfr::error::ok);
  soup::login_request request;
  REQUIRE(soup::decode_login_request(frame.payload).get(request) ==
          dfr::error::ok);
  CHECK(request.username == "USER01");
  CHECK(request.password == "SECRET");
  CHECK(request.requested_session == "SESS01");
  CHECK(request.requested_sequence == 1);
}

TEST_CASE("an empty requested session means the active one",
          "[wire][soupbintcp]") {
  // All spaces on the wire, and the absence *is* the meaning, so it decodes as empty rather than as
  // a sentinel a caller has to know about.
  std::array<std::byte, 128> buffer{};
  const dfr::mutable_packet_view out{buffer.data(), buffer.size()};
  std::size_t written = 0;
  REQUIRE(soup::encode_login_request(out, "USER01", "PW", "", 0).get(written) ==
          dfr::error::ok);

  soup::packet frame;
  REQUIRE(soup::decode(dfr::packet_view{buffer.data(), written}).get(frame) ==
          dfr::error::ok);
  soup::login_request request;
  REQUIRE(soup::decode_login_request(frame.payload).get(request) ==
          dfr::error::ok);
  CHECK(request.requested_session.empty());
  CHECK(request.requested_sequence == 0);
}

TEST_CASE("a rejection reason is decoded or reported unknown",
          "[wire][soupbintcp]") {
  // Mapping an unrecognised byte onto one of the two would make a client retry a login it should
  // have abandoned, or abandon one it should have retried.
  std::array<std::byte, 16> buffer{};
  const dfr::mutable_packet_view out{buffer.data(), buffer.size()};
  std::size_t written = 0;
  REQUIRE(soup::encode_login_rejected(out, soup::reject_reason::not_authorized)
              .get(written) == dfr::error::ok);

  soup::packet frame;
  REQUIRE(soup::decode(dfr::packet_view{buffer.data(), written}).get(frame) ==
          dfr::error::ok);
  soup::reject_reason reason{};
  REQUIRE(soup::decode_login_rejected(frame.payload).get(reason) ==
          dfr::error::ok);
  CHECK(reason == soup::reject_reason::not_authorized);

  dfr_test::soup::raw_stream odd;
  odd.frame('J', "?");
  soup::packet other;
  REQUIRE(soup::decode(odd.view()).get(other) == dfr::error::ok);
  CHECK(soup::decode_login_rejected(other.payload).error_code() ==
        dfr::error::not_supported);
}

// ---------------------------------------------------------------------------
// The implicit sequence
// ---------------------------------------------------------------------------

TEST_CASE("sequenced packets are numbered from the login position",
          "[wire][soupbintcp]") {
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "one").frame('S', "two").frame('S', "three");

  soup::stream_cursor cursor{100};
  const auto seen = walk(cursor, stream.view());
  REQUIRE(seen.packets.size() == 3);
  CHECK(seen.packets[0].sequence == 100);
  CHECK(seen.packets[1].sequence == 101);
  CHECK(seen.packets[2].sequence == 102);
  CHECK(cursor.next_sequence() == 103);
  CHECK(cursor.sequenced_packets() == 3);
}

TEST_CASE("only sequenced packets advance the count",
          "[wire][soupbintcp][regression]") {
  // A debug packet or a heartbeat between two sequenced ones must not shift the numbering. Nothing on
  // the wire states a position, so an implementation that counted them would be wrong from that point
  // on with no way to notice.
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "one")
      .frame('H', "")
      .frame('+', "a debug message")
      .frame('S', "two")
      .frame('H', "")
      .frame('S', "three");

  soup::stream_cursor cursor{1};
  const auto seen = walk(cursor, stream.view());
  REQUIRE(seen.packets.size() == 6);
  CHECK(seen.packets[0].sequence == 1);
  CHECK(seen.packets[1].sequence == 0);  // a heartbeat has no position, not an unknown one
  CHECK(seen.packets[2].sequence == 0);
  CHECK(seen.packets[3].sequence == 2);
  CHECK(seen.packets[5].sequence == 3);
  CHECK(cursor.sequenced_packets() == 3);
  CHECK(cursor.packets() == 6);
}

TEST_CASE("sequenced data before login is refused, not guessed",
          "[wire][soupbintcp]") {
  // A client that has not logged in cannot number what it is receiving, and inventing a position
  // would give every message a number that looks authoritative.
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "data");

  soup::stream_cursor cursor;
  soup::sequenced_packet packet;
  CHECK(cursor.next(stream.view()).get(packet) == dfr::error::sequence_reset);
}

TEST_CASE("a session can adopt its position after the cursor exists",
          "[wire][soupbintcp]") {
  // A session is usually built before it has logged in, and a cursor reconstructed at that moment
  // would lose the counts it had already taken.
  dfr_test::soup::raw_stream stream;
  stream.frame('H', "").login_accepted("SESS", "500").frame('S', "first");

  soup::stream_cursor cursor;
  std::size_t at = 0;
  for (int step = 0; step < 3; ++step) {
    dfr::packet_view rest;
    REQUIRE(stream.view().subview(at, stream.size() - at).get(rest) ==
            dfr::error::ok);
    soup::sequenced_packet packet;
    REQUIRE(cursor.next(rest).get(packet) == dfr::error::ok);
    at += packet.frame.frame_size;

    if (packet.frame.type == soup::packet_type::login_accepted) {
      soup::login_accepted accepted;
      REQUIRE(soup::decode_login_accepted(packet.frame.payload).get(accepted) ==
              dfr::error::ok);
      cursor.accept_login(accepted.next_sequence);
    } else if (packet.frame.type == soup::packet_type::sequenced_data) {
      CHECK(packet.sequence == 500);
    }
  }
  CHECK(cursor.packets() == 3);
  CHECK(cursor.next_sequence() == 501);
}

TEST_CASE("a partial packet consumes nothing and can be retried",
          "[wire][soupbintcp][regression]") {
  // The discipline capture::pcap::reader follows for a truncated record, for the same reason: a
  // failed read that had advanced the cursor would leave nothing behind to retry with, and on a
  // stream the retry is the normal path rather than the exceptional one.
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "one").frame('S', "a longer second message");

  soup::stream_cursor cursor{7};
  const auto partial = walk(cursor, stream.prefix(stream.size() - 4));
  CHECK(partial.stopped == dfr::error::need_more_bytes);
  REQUIRE(partial.packets.size() == 1);
  CHECK(partial.packets[0].sequence == 7);

  // Now the rest arrives. The consumed prefix is dropped and the remainder retried.
  dfr::packet_view rest;
  REQUIRE(stream.view()
              .subview(partial.consumed, stream.size() - partial.consumed)
              .get(rest) == dfr::error::ok);
  const auto finished = walk(cursor, rest);
  CHECK(finished.stopped == dfr::error::ok);
  REQUIRE(finished.packets.size() == 1);
  CHECK(finished.packets[0].sequence == 8);
  CHECK(cursor.sequenced_packets() == 2);
}

TEST_CASE("ending a session forgets the position but not the counts",
          "[wire][soupbintcp]") {
  // The counts describe the connection, and a caller reporting how much a session carried before it
  // dropped needs them after it has dropped.
  dfr_test::soup::raw_stream stream;
  stream.frame('S', "one").frame('Z', "");

  soup::stream_cursor cursor{1};
  const auto seen = walk(cursor, stream.view());
  REQUIRE(seen.packets.size() == 2);

  cursor.end_session();
  CHECK_FALSE(cursor.started());
  CHECK(cursor.packets() == 2);
  CHECK(cursor.sequenced_packets() == 1);
}

#include <dfr/wire/moldudp64.hpp>

#include "support/death_test.hpp"
#include "wire/support/raw_moldudp64.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mold = dfr::wire::moldudp64;
using dfr_test::mold::as_text;
using dfr_test::mold::raw_packet;
using dfr_test::mold::three_messages;


// ---------------------------------------------------------------------------
// Wire constants
// ---------------------------------------------------------------------------

TEST_CASE("the header layout matches the specification", "[wire][moldudp64]") {
  STATIC_REQUIRE(mold::kHeaderSize == 20);
  STATIC_REQUIRE(mold::kSessionOffset == 0);
  STATIC_REQUIRE(mold::kSessionSize == 10);
  STATIC_REQUIRE(mold::kSequenceOffset == 10);
  STATIC_REQUIRE(mold::kSequenceSize == 8);
  STATIC_REQUIRE(mold::kMessageCountOffset == 18);
  STATIC_REQUIRE(mold::kMessageCountSize == 2);
  STATIC_REQUIRE(mold::kHeartbeat == 0);
  STATIC_REQUIRE(mold::kEndOfSession == 0xFFFF);
  STATIC_REQUIRE(mold::kMaxMessagesPerRequest == 60'000);
}

// ---------------------------------------------------------------------------
// session_id
// ---------------------------------------------------------------------------

TEST_CASE("a session id is space-padded to ten bytes", "[wire][moldudp64]") {
  mold::session_id id;
  REQUIRE(mold::session_id::from_text("ABC").get(id) == dfr::error::ok);

  const auto bytes = id.bytes();
  CHECK(bytes[0] == static_cast<std::byte>('A'));
  CHECK(bytes[2] == static_cast<std::byte>('C'));
  CHECK(bytes[3] == static_cast<std::byte>(' '));
  CHECK(bytes[9] == static_cast<std::byte>(' '));

  CHECK(id.text() == "ABC");
}

TEST_CASE("an over-long session name is rejected, not truncated",
          "[wire][moldudp64]") {
  // Truncating would make two distinct sessions compare equal, and a session
  // change is a fatal error, so this is the one place where being strict is
  // cheaper than being forgiving.
  const auto too_long = mold::session_id::from_text("ELEVENCHARS");
  CHECK_FALSE(too_long.has_value());
  CHECK(too_long.error_code() == dfr::error::invalid_argument);

  CHECK(mold::session_id::from_text("TENCHARSXX").has_value());
}

TEST_CASE("session ids compare byte-exactly, including padding",
          "[wire][moldudp64]") {
  mold::session_id spaces;
  mold::session_id nulls;
  REQUIRE(mold::session_id::from_text("A").get(spaces) == dfr::error::ok);

  std::array<std::byte, mold::kSessionSize> null_padded{};
  null_padded[0] = static_cast<std::byte>('A');
  nulls = mold::session_id{null_padded};

  // Both "mean" session A. They are not equal, deliberately: if a publisher
  // ever pads with nulls instead of spaces that is a finding to report, not an
  // equivalence to invent inside a comparison operator.
  CHECK_FALSE(spaces == nulls);

  mold::session_id same;
  REQUIRE(mold::session_id::from_text("A").get(same) == dfr::error::ok);
  CHECK(spaces == same);
}

TEST_CASE("a default session id round-trips as blanks", "[wire][moldudp64]") {
  const mold::session_id id;
  CHECK(id.text().empty());
  for (const std::byte b : id.bytes()) {
    CHECK(b == static_cast<std::byte>(' '));
  }
}

// ---------------------------------------------------------------------------
// decode_header
// ---------------------------------------------------------------------------

TEST_CASE("a header decodes from known bytes", "[wire][moldudp64]") {
  const auto packet = three_messages();

  mold::header decoded;
  REQUIRE(mold::decode_header(packet.view()).get(decoded) == dfr::error::ok);

  CHECK(decoded.session.text() == "SESS01");
  CHECK(decoded.sequence == 100);
  CHECK(decoded.message_count == 3);
  CHECK(decoded.kind() == mold::packet_kind::data);
}

TEST_CASE("a datagram shorter than the header is truncated_header",
          "[wire][moldudp64]") {
  // Its own code rather than block_overruns_datagram: nothing about a block is
  // wrong, the packet never had a header to begin with.
  const std::array<std::uint8_t, 19> nineteen{};
  const auto result =
      mold::decode_header(dfr::packet_view{nineteen.data(), nineteen.size()});

  CHECK_FALSE(result.has_value());
  CHECK(result.error_code() == dfr::error::truncated_header);

  CHECK_FALSE(mold::decode_header(dfr::packet_view{}).has_value());
}

TEST_CASE("a header with no blocks decodes", "[wire][moldudp64]") {
  const auto packet = raw_packet{}.session("S").sequence(7).count(0);
  REQUIRE(packet.size() == mold::kHeaderSize);

  mold::header decoded;
  REQUIRE(mold::decode_header(packet.view()).get(decoded) == dfr::error::ok);
  CHECK(decoded.kind() == mold::packet_kind::heartbeat);
}

// ---------------------------------------------------------------------------

// The semantic implementations get wrong
// ---------------------------------------------------------------------------

TEST_CASE("sequence numbers count messages, not packets",
          "[wire][moldudp64]") {
  // The single most important semantic in the protocol. A packet at sequence
  // 100 carrying three messages occupies 100, 101 and 102, so the next packet
  // begins at 103. An implementation that increments by one per packet falls
  // behind by (count - 1) every packet.
  const mold::header three{.sequence = 100, .message_count = 3};
  CHECK(three.next_sequence() == 103);

  const mold::header one{.sequence = 100, .message_count = 1};
  CHECK(one.next_sequence() == 101);

  const mold::header many{.sequence = 1, .message_count = 500};
  CHECK(many.next_sequence() == 501);
}

TEST_CASE("a heartbeat advances to its sequence, not past it",
          "[wire][moldudp64]") {
  // A heartbeat's Sequence Number is the *next* sequence the publisher will
  // send, so it advances a watermark rather than occupying a slot. Treating it
  // as a data packet at that sequence reports a gap that does not exist.
  const mold::header beat{.sequence = 500, .message_count = mold::kHeartbeat};

  CHECK(beat.kind() == mold::packet_kind::heartbeat);
  CHECK(beat.next_sequence() == 500);
}

TEST_CASE("end of session is a count, not a length", "[wire][moldudp64]") {
  const mold::header last{.sequence = 900,
                          .message_count = mold::kEndOfSession};

  CHECK(last.kind() == mold::packet_kind::end_of_session);
  CHECK(last.next_sequence() == 900);

  // 0xFFFF must not be read as "65535 messages", which is what an
  // implementation that only special-cases zero would do.
  CHECK(last.next_sequence() != 900 + 0xFFFF);
}

TEST_CASE("packet kinds have distinct names", "[wire][moldudp64]") {
  CHECK(mold::to_string(mold::packet_kind::data) == "data");
  CHECK(mold::to_string(mold::packet_kind::heartbeat) == "heartbeat");
  CHECK(mold::to_string(mold::packet_kind::end_of_session) == "end_of_session");
}

// ---------------------------------------------------------------------------
// message_cursor: the happy path
// ---------------------------------------------------------------------------

TEST_CASE("the cursor walks blocks and numbers them", "[wire][moldudp64]") {
  const auto packet = three_messages();

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  CHECK(cursor.remaining() == 3);
  CHECK_FALSE(cursor.done());

  mold::message first;
  REQUIRE(cursor.next().get(first) == dfr::error::ok);
  CHECK(first.sequence == 100);
  CHECK(as_text(first.payload) == "aaa");

  mold::message second;
  REQUIRE(cursor.next().get(second) == dfr::error::ok);
  CHECK(second.sequence == 101);
  CHECK(as_text(second.payload) == "bb");

  mold::message third;
  REQUIRE(cursor.next().get(third) == dfr::error::ok);
  CHECK(third.sequence == 102);
  CHECK(as_text(third.payload) == "c");

  CHECK(cursor.done());
  CHECK(cursor.rest().empty());
}

TEST_CASE("a heartbeat cursor is born exhausted", "[wire][moldudp64]") {
  // So that the ordinary `while (!done())` loop handles heartbeats and
  // end-of-session without a special case at every call site.
  for (const std::uint16_t count : {mold::kHeartbeat, mold::kEndOfSession}) {
    const auto packet = raw_packet{}.session("S").sequence(9).count(count);

    mold::message_cursor cursor;
    REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
            dfr::error::ok);

    CHECK(cursor.done());
    CHECK(cursor.remaining() == 0);
  }
}

TEST_CASE("a zero-length block is legal", "[wire][moldudp64]") {
  // Legal on the wire, and specifically *not* the end-of-session marker: that
  // is signalled by Message Count. A decoder that treats a zero length as the
  // end silently truncates the rest of the datagram.
  const auto packet = raw_packet{}
                          .session("S")
                          .sequence(1)
                          .count(2)
                          .block("")
                          .block("x");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  mold::message empty;
  REQUIRE(cursor.next().get(empty) == dfr::error::ok);
  CHECK(empty.sequence == 1);
  CHECK(empty.payload.empty());

  mold::message present;
  REQUIRE(cursor.next().get(present) == dfr::error::ok);
  CHECK(present.sequence == 2);
  CHECK(as_text(present.payload) == "x");
  CHECK(cursor.done());
}

TEST_CASE("drain collects every message", "[wire][moldudp64]") {
  const auto packet = three_messages();

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  std::vector<std::uint64_t> sequences;
  const auto outcome =
      cursor.drain([&](const mold::message& m) { sequences.push_back(m.sequence); });

  CHECK(outcome.has_value());
  CHECK(sequences == std::vector<std::uint64_t>{100, 101, 102});
}


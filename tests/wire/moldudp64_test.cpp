#include <dfr/wire/moldudp64.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mold = dfr::wire::moldudp64;

namespace {

// Builds a datagram byte by byte, so a test can produce malformed packets that
// the library's own encoder is designed to be incapable of producing.
class raw_packet {
 public:
  raw_packet& session(std::string_view name) {
    for (std::size_t i = 0; i < mold::kSessionSize; ++i) {
      bytes_.push_back(i < name.size() ? static_cast<std::uint8_t>(name[i])
                                       : std::uint8_t{' '});
    }
    return *this;
  }

  raw_packet& sequence(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
    return *this;
  }

  raw_packet& count(std::uint16_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    return *this;
  }

  // A well-formed block: a truthful length followed by that many bytes.
  raw_packet& block(std::string_view payload) {
    return declared_block(static_cast<std::uint16_t>(payload.size()), payload);
  }

  // A block whose declared length need not match what follows, which is how the
  // second half of the helix defect is reproduced.
  raw_packet& declared_block(std::uint16_t declared, std::string_view payload) {
    bytes_.push_back(static_cast<std::uint8_t>(declared >> 8));
    bytes_.push_back(static_cast<std::uint8_t>(declared & 0xFF));
    for (const char c : payload) {
      bytes_.push_back(static_cast<std::uint8_t>(c));
    }
    return *this;
  }

  raw_packet& raw_byte(std::uint8_t value) {
    bytes_.push_back(value);
    return *this;
  }

  [[nodiscard]] dfr::packet_view view() const {
    return {bytes_.data(), bytes_.size()};
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

std::string_view as_text(dfr::packet_view v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

// A well-formed three-message packet at sequence 100.
raw_packet three_messages() {
  return raw_packet{}
      .session("SESS01")
      .sequence(100)
      .count(3)
      .block("aaa")
      .block("bb")
      .block("c");
}

}  // namespace

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
  // change is a fatal error — so this is the one place where being strict is
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
// message_cursor — the happy path
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
  // Legal on the wire, and specifically *not* the end-of-session marker — that
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

// ---------------------------------------------------------------------------
// The helix defect, on the real protocol
// ---------------------------------------------------------------------------

TEST_CASE("an overstated block count is caught, not trusted",
          "[wire][moldudp64][regression]") {
  // penberg/helix moldudp64.hh:106-115 loops `MessageCount` times performing a
  // reinterpret_cast with no bounds check. Here the header claims four blocks
  // and two are present. The cursor must report it on the third attempt rather
  // than reading whatever follows the buffer.
  const auto packet = raw_packet{}
                          .session("LIAR")
                          .sequence(50)
                          .count(4)
                          .block("aa")
                          .block("bb");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  mold::message m;
  REQUIRE(cursor.next().get(m) == dfr::error::ok);
  CHECK(m.sequence == 50);
  REQUIRE(cursor.next().get(m) == dfr::error::ok);
  CHECK(m.sequence == 51);

  REQUIRE_FALSE(cursor.done());  // the header still claims two more
  const auto third = cursor.next();
  CHECK_FALSE(third.has_value());
  CHECK(third.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("drain reports an overstated count", "[wire][moldudp64][regression]") {
  const auto packet =
      raw_packet{}.session("LIAR").sequence(1).count(9).block("only");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const mold::message&) { ++delivered; });

  CHECK(delivered == 1);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("a block whose declared length overruns is caught",
          "[wire][moldudp64][regression]") {
  // The other half of the same defect: the count is honest, the length is not.
  const auto packet = raw_packet{}
                          .session("LIAR")
                          .sequence(1)
                          .count(1)
                          .declared_block(0xFFFF, "short");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  const auto only = cursor.next();
  CHECK_FALSE(only.has_value());
  CHECK(only.error_code() == dfr::error::block_overruns_datagram);
}

TEST_CASE("a truncated length field is an overstated count",
          "[wire][moldudp64][regression]") {
  // One stray byte where a two-byte length should be. The header promised a
  // block and there is not even room for its length, so the count is the lie.
  const auto packet =
      raw_packet{}.session("S").sequence(1).count(1).raw_byte(0x00);

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  const auto only = cursor.next();
  CHECK_FALSE(only.has_value());
  CHECK(only.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("an understated count leaves trailing bytes",
          "[wire][moldudp64][regression]") {
  // The mirror image, and just as damaging: the header says one block, two are
  // present, and the second is silently dropped by any decoder that stops
  // counting. drain() is what catches it.
  const auto packet = raw_packet{}
                          .session("S")
                          .sequence(1)
                          .count(1)
                          .block("first")
                          .block("second");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const mold::message&) { ++delivered; });

  CHECK(delivered == 1);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("calling next() on an exhausted cursor is a programmer error",
          "[wire][moldudp64]") {
  DFR_CHECK_ABORTS({
    const auto packet = raw_packet{}.session("S").sequence(1).count(0);
    mold::message_cursor cursor;
    static_cast<void>(mold::message_cursor::over(packet.view()).get(cursor));
    static_cast<void>(cursor.next());
  });
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

TEST_CASE("a header round-trips through encode and decode",
          "[wire][moldudp64]") {
  mold::session_id session;
  REQUIRE(mold::session_id::from_text("RTRIP").get(session) == dfr::error::ok);

  const mold::header original{
      .session = session, .sequence = 0xDEAD'BEEF'CAFE'BABE, .message_count = 7};

  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  std::size_t written = 0;
  REQUIRE(mold::encode_header(dfr::mutable_packet_view{buffer.data(),
                                                       buffer.size()},
                              original)
              .get(written) == dfr::error::ok);
  CHECK(written == mold::kHeaderSize);

  mold::header decoded;
  REQUIRE(mold::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(decoded) == dfr::error::ok);
  CHECK(decoded == original);
}

TEST_CASE("encoding into too small a buffer fails without writing",
          "[wire][moldudp64]") {
  std::array<std::uint8_t, mold::kHeaderSize - 1> small{};
  const auto outcome = mold::encode_header(
      dfr::mutable_packet_view{small.data(), small.size()}, mold::header{});

  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::capacity_exceeded);
  for (const std::uint8_t b : small) {
    CHECK(b == 0);
  }
}

TEST_CASE("the builder writes the count on finish", "[wire][moldudp64]") {
  // The builder cannot produce a packet whose count disagrees with its contents,
  // because the count is not an input. That defect is one dfr::chaos will inject
  // deliberately, so the honest encoder must be incapable of it by accident.
  mold::session_id session;
  REQUIRE(mold::session_id::from_text("BUILD").get(session) == dfr::error::ok);

  std::array<std::uint8_t, 128> buffer{};
  mold::packet_builder builder{
      *mold::packet_builder::into(
          dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 400)};

  CHECK(builder.message_count() == 0);
  CHECK(builder.size() == mold::kHeaderSize);

  const std::array<std::string_view, 3> bodies{"alpha", "b", "gamma!"};
  for (const std::string_view body : bodies) {
    REQUIRE(builder.append(dfr::packet_view{body.data(), body.size()})
                .has_value());
  }
  CHECK(builder.message_count() == 3);

  dfr::packet_view finished;
  REQUIRE(builder.finish().get(finished) == dfr::error::ok);

  // Decode what was built and check every field survived.
  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(finished).get(cursor) == dfr::error::ok);
  CHECK(cursor.packet_header().session == session);
  CHECK(cursor.packet_header().sequence == 400);
  CHECK(cursor.packet_header().message_count == 3);
  CHECK(cursor.packet_header().next_sequence() == 403);

  std::vector<std::string_view> seen;
  std::vector<std::uint64_t> sequences;
  REQUIRE(cursor
              .drain([&](const mold::message& m) {
                seen.push_back(as_text(m.payload));
                sequences.push_back(m.sequence);
              })
              .has_value());

  CHECK(seen == std::vector<std::string_view>{"alpha", "b", "gamma!"});
  CHECK(sequences == std::vector<std::uint64_t>{400, 401, 402});
}

TEST_CASE("the builder refuses a message it cannot fit",
          "[wire][moldudp64]") {
  mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize + 4> buffer{};
  mold::packet_builder builder{
      *mold::packet_builder::into(
          dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 1)};

  // Two bytes of length plus two of payload exactly fills it.
  REQUIRE(builder.append(dfr::packet_view{"ab", 2}).has_value());
  CHECK(builder.size() == buffer.size());

  const auto rejected = builder.append(dfr::packet_view{"c", 1});
  CHECK_FALSE(rejected.has_value());
  CHECK(rejected.error_code() == dfr::error::capacity_exceeded);

  // And the rejection left the packet intact, so finish() still works.
  CHECK(builder.message_count() == 1);
  CHECK(builder.finish().has_value());
}

TEST_CASE("a heartbeat and an end-of-session encode to header-only packets",
          "[wire][moldudp64]") {
  mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  const dfr::mutable_packet_view out{buffer.data(), buffer.size()};

  REQUIRE(mold::encode_heartbeat(out, session, 777).has_value());
  mold::header beat;
  REQUIRE(mold::decode_header(out.as_const()).get(beat) == dfr::error::ok);
  CHECK(beat.kind() == mold::packet_kind::heartbeat);
  CHECK(beat.sequence == 777);
  CHECK(beat.next_sequence() == 777);

  REQUIRE(mold::encode_end_of_session(out, session, 888).has_value());
  mold::header last;
  REQUIRE(mold::decode_header(out.as_const()).get(last) == dfr::error::ok);
  CHECK(last.kind() == mold::packet_kind::end_of_session);
  CHECK(last.sequence == 888);
}

// ---------------------------------------------------------------------------
// Retransmission requests
// ---------------------------------------------------------------------------

TEST_CASE("a request count is clamped to the protocol maximum",
          "[wire][moldudp64]") {
  // The facility rejects an over-large request outright rather than truncating
  // it, so a client that asks for a million messages receives nothing and then
  // reports a timeout it caused itself.
  STATIC_REQUIRE(mold::clamp_request_count(1) == 1);
  STATIC_REQUIRE(mold::clamp_request_count(60'000) == 60'000);
  STATIC_REQUIRE(mold::clamp_request_count(60'001) == 60'000);
  STATIC_REQUIRE(mold::clamp_request_count(1'000'000) == 60'000);
  STATIC_REQUIRE(mold::clamp_request_count(UINT64_MAX) == 60'000);

  // And the clamp must never land on the end-of-session sentinel.
  STATIC_REQUIRE(mold::clamp_request_count(UINT64_MAX) != mold::kEndOfSession);
}

TEST_CASE("a request encodes as a header with the wanted range",
          "[wire][moldudp64]") {
  mold::session_id session;
  REQUIRE(mold::session_id::from_text("REQ").get(session) == dfr::error::ok);

  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  REQUIRE(mold::encode_request(
              dfr::mutable_packet_view{buffer.data(), buffer.size()}, session,
              1000, 250)
              .has_value());

  mold::header request;
  REQUIRE(mold::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(request) == dfr::error::ok);
  CHECK(request.session == session);
  CHECK(request.sequence == 1000);
  CHECK(request.message_count == 250);
}

TEST_CASE("a request is clamped when encoded", "[wire][moldudp64]") {
  mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  REQUIRE(mold::encode_request(
              dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 1,
              5'000'000)
              .has_value());

  mold::header request;
  REQUIRE(mold::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(request) == dfr::error::ok);
  CHECK(request.message_count == mold::kMaxMessagesPerRequest);
}

TEST_CASE("a zero-count request is rejected", "[wire][moldudp64]") {
  // Asking for nothing is always a caller bug rather than a protocol state, and
  // on the wire it would be indistinguishable from a heartbeat.
  mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  const auto outcome = mold::encode_request(
      dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 1, 0);

  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// Compile-time use
// ---------------------------------------------------------------------------

TEST_CASE("a header decodes at compile time", "[wire][moldudp64]") {
  // Which means a wire-format expectation can be pinned in a static_assert,
  // with no fixture and no runtime at all.
  static constexpr std::array<std::byte, mold::kHeaderSize> bytes{
      std::byte{'S'},  std::byte{' '},  std::byte{' '},  std::byte{' '},
      std::byte{' '},  std::byte{' '},  std::byte{' '},  std::byte{' '},
      std::byte{' '},  std::byte{' '},  std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x64}, std::byte{0x00}, std::byte{0x03}};

  static constexpr dfr::packet_view packet{bytes.data(), bytes.size()};
  constexpr auto decoded = mold::decode_header(packet);

  STATIC_REQUIRE(decoded.has_value());
  STATIC_REQUIRE(decoded.value_unsafe().sequence == 100);
  STATIC_REQUIRE(decoded.value_unsafe().message_count == 3);
  STATIC_REQUIRE(decoded.value_unsafe().next_sequence() == 103);
  STATIC_REQUIRE(decoded.value_unsafe().kind() == mold::packet_kind::data);
}

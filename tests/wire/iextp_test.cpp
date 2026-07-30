#include <dfr/wire/iextp.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace iex = dfr::wire::iextp;

namespace {

// Assembles a datagram byte by byte, little-endian, so a test can produce
// packets the library's own encoder is designed to be unable to produce.
class raw_packet {
 public:
  raw_packet() {
    // Version 1, reserved 0. Overridable via version().
    bytes_.assign(iex::kHeaderSize, 0);
    bytes_[iex::kVersionOffset] = iex::kVersion;
  }

  raw_packet& version(std::uint8_t v) {
    bytes_[iex::kVersionOffset] = v;
    return *this;
  }
  raw_packet& protocol(std::uint16_t v) { return put_le(iex::kProtocolIdOffset, v); }
  raw_packet& channel(std::uint32_t v) { return put_le(iex::kChannelIdOffset, v); }
  raw_packet& session(std::uint32_t v) { return put_le(iex::kSessionIdOffset, v); }
  raw_packet& payload_length(std::uint16_t v) {
    return put_le(iex::kPayloadLengthOffset, v);
  }
  raw_packet& count(std::uint16_t v) { return put_le(iex::kMessageCountOffset, v); }
  raw_packet& stream_offset(std::int64_t v) {
    return put_le(iex::kStreamOffsetOffset, static_cast<std::uint64_t>(v));
  }
  raw_packet& first_sequence(std::uint64_t v) {
    return put_le(iex::kFirstSequenceOffset, v);
  }
  raw_packet& send_time(std::uint64_t v) { return put_le(iex::kSendTimeOffset, v); }

  // A truthful block: its declared length matches what follows.
  raw_packet& block(std::string_view payload) {
    return declared_block(static_cast<std::uint16_t>(payload.size()), payload);
  }

  raw_packet& declared_block(std::uint16_t declared, std::string_view payload) {
    bytes_.push_back(static_cast<std::uint8_t>(declared & 0xFF));
    bytes_.push_back(static_cast<std::uint8_t>(declared >> 8));
    for (const char c : payload) {
      bytes_.push_back(static_cast<std::uint8_t>(c));
    }
    return *this;
  }

  raw_packet& raw_byte(std::uint8_t v) {
    bytes_.push_back(v);
    return *this;
  }

  // Sets Payload Length to whatever was actually appended, which is what a
  // truthful publisher would do.
  raw_packet& seal() {
    return payload_length(
        static_cast<std::uint16_t>(bytes_.size() - iex::kHeaderSize));
  }

  [[nodiscard]] dfr::packet_view view() const {
    return {bytes_.data(), bytes_.size()};
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

 private:
  template <typename T>
  raw_packet& put_le(std::size_t at, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes_[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return *this;
  }

  std::vector<std::uint8_t> bytes_;
};

std::string_view as_text(dfr::packet_view v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

iex::header prototype() {
  return iex::header{.protocol = static_cast<std::uint16_t>(iex::protocol_id::deep),
                     .channel = 1,
                     .session = 0xABCD,
                     .stream_offset = 0,
                     .first_sequence = 1,
                     .send_time_ns = 1'700'000'000'000'000'000ULL};
}

}  // namespace

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
// The chains
// ---------------------------------------------------------------------------

TEST_CASE("sequence and stream offset chain independently", "[wire][iextp]") {
  const iex::header first{
      .payload_length = 12, .message_count = 3, .stream_offset = 100,
      .first_sequence = 1000};

  CHECK(first.next_sequence() == 1003);
  CHECK(first.next_stream_offset() == 112);
}

TEST_CASE("the chain checker accepts a well-formed sequence",
          "[wire][iextp]") {
  iex::chain_checker checker;
  CHECK_FALSE(checker.started());

  iex::header packet{.payload_length = 10,
                     .message_count = 2,
                     .stream_offset = 0,
                     .first_sequence = 1};

  for (int i = 0; i < 5; ++i) {
    REQUIRE(checker.observe(packet).has_value());
    packet.first_sequence = packet.next_sequence();
    packet.stream_offset = packet.next_stream_offset();
  }

  CHECK(checker.started());
  CHECK(checker.expected_sequence() == 11);
  CHECK(checker.expected_stream_offset() == 50);
}

TEST_CASE("the first packet establishes the chain rather than failing",
          "[wire][iextp]") {
  // A receiver joining a live feed mid-session must not report a spurious error
  // on its first packet.
  iex::chain_checker checker;
  const iex::header mid_session{.payload_length = 8,
                                .message_count = 1,
                                .stream_offset = 987'654,
                                .first_sequence = 555'000};

  CHECK(checker.observe(mid_session).has_value());
  CHECK(checker.expected_sequence() == 555'001);
}

TEST_CASE("a sequence gap is reported and then resynchronised",
          "[wire][iextp]") {
  iex::chain_checker checker;
  const iex::header first{
      .payload_length = 4, .message_count = 1, .stream_offset = 0,
      .first_sequence = 1};
  REQUIRE(checker.observe(first).has_value());

  // Jumps from expected 2 to 10.
  const iex::header jumped{
      .payload_length = 4, .message_count = 1, .stream_offset = 4,
      .first_sequence = 10};
  const auto gap = checker.observe(jumped);
  CHECK_FALSE(gap.has_value());
  CHECK(gap.error_code() == dfr::error::sequence_gap);
  CHECK_FALSE(gap.is_fatal());

  // Resynchronised, so one gap does not produce an error on every packet after
  // it — which would bury the next real fault in noise.
  const iex::header following{
      .payload_length = 4, .message_count = 1, .stream_offset = 8,
      .first_sequence = 11};
  CHECK(checker.observe(following).has_value());
}

TEST_CASE("a regressed sequence is distinguished from a gap",
          "[wire][iextp]") {
  // A duplicate, or the late half of an A/B pair. Both recoverable and usually
  // uninteresting, so they must not be reported as a gap that triggers recovery.
  iex::chain_checker checker;
  const iex::header first{
      .payload_length = 4, .message_count = 2, .stream_offset = 0,
      .first_sequence = 100};
  REQUIRE(checker.observe(first).has_value());

  const auto replayed = checker.observe(first);
  CHECK_FALSE(replayed.has_value());
  CHECK(replayed.error_code() == dfr::error::sequence_regressed);
  CHECK_FALSE(replayed.is_fatal());
}

TEST_CASE("a session change is fatal", "[wire][iextp]") {
  iex::chain_checker checker;
  const iex::header first{.session = 1,
                          .payload_length = 4,
                          .message_count = 1,
                          .stream_offset = 0,
                          .first_sequence = 1};
  REQUIRE(checker.observe(first).has_value());

  const iex::header new_session{.session = 2,
                                .payload_length = 4,
                                .message_count = 1,
                                .stream_offset = 0,
                                .first_sequence = 1};
  const auto changed = checker.observe(new_session);
  CHECK_FALSE(changed.has_value());
  CHECK(changed.error_code() == dfr::error::session_changed);
  CHECK(changed.is_fatal());
}

TEST_CASE("a broken offset chain is caught when the sequence chain is intact",
          "[wire][iextp][oracle]") {
  // The reason to build against IEX first, in one test.
  //
  // Sequence numbers chain perfectly and stream offsets do not, which is what a
  // corrupted Payload Length looks like. A receiver checking only sequence
  // numbers — which is all MoldUDP64 permits — sees nothing wrong and carries a
  // silently wrong byte position for the rest of the session.
  iex::chain_checker checker;
  const iex::header first{
      .payload_length = 10, .message_count = 1, .stream_offset = 0,
      .first_sequence = 1};
  REQUIRE(checker.observe(first).has_value());

  const iex::header offset_wrong{
      .payload_length = 10,
      .message_count = 1,
      .stream_offset = 999,  // should be 10
      .first_sequence = 2};  // chains correctly

  const auto broken = checker.observe(offset_wrong);
  CHECK_FALSE(broken.has_value());
  CHECK(broken.error_code() == dfr::error::message_length_mismatch);
}

// ---------------------------------------------------------------------------
// message_cursor
// ---------------------------------------------------------------------------

TEST_CASE("the cursor walks blocks and numbers them", "[wire][iextp]") {
  const auto packet = raw_packet{}
                          .first_sequence(500)
                          .count(3)
                          .block("aaa")
                          .block("bb")
                          .block("c")
                          .seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);
  CHECK(cursor.remaining() == 3);

  std::vector<std::uint64_t> sequences;
  std::vector<std::string_view> payloads;
  REQUIRE(cursor
              .drain([&](const iex::message& m) {
                sequences.push_back(m.sequence);
                payloads.push_back(as_text(m.payload));
              })
              .has_value());

  CHECK(sequences == std::vector<std::uint64_t>{500, 501, 502});
  CHECK(payloads == std::vector<std::string_view>{"aaa", "bb", "c"});
}

TEST_CASE("a heartbeat cursor is born exhausted", "[wire][iextp]") {
  const auto packet = raw_packet{}.first_sequence(9).count(0).seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);
  CHECK(cursor.done());
  CHECK(cursor.packet_header().kind() == iex::packet_kind::heartbeat);
}

TEST_CASE("the cursor trusts the declared payload length, not the datagram size",
          "[wire][iextp]") {
  // A switch may pad a short frame and a capture may truncate one. Trusting the
  // datagram length would decode the wrong number of blocks in either case.
  const auto padded = raw_packet{}
                          .first_sequence(1)
                          .count(1)
                          .block("ab")
                          .seal()      // payload length = 4
                          .raw_byte(0xFF)  // one byte of padding after it
                          .raw_byte(0xFF);

  const auto decoded = iex::message_cursor::over(padded.view());
  CHECK_FALSE(decoded.has_value());
  CHECK(decoded.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("a payload length longer than the datagram is rejected",
          "[wire][iextp]") {
  const auto packet =
      raw_packet{}.first_sequence(1).count(1).block("ab").payload_length(500);

  const auto decoded = iex::message_cursor::over(packet.view());
  CHECK_FALSE(decoded.has_value());
  CHECK(decoded.error_code() == dfr::error::block_overruns_datagram);
}

TEST_CASE("an overstated block count is caught", "[wire][iextp][regression]") {
  const auto packet =
      raw_packet{}.first_sequence(1).count(4).block("aa").block("bb").seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const iex::message&) { ++delivered; });
  CHECK(delivered == 2);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("a block whose length overruns the payload is caught",
          "[wire][iextp][regression]") {
  const auto packet = raw_packet{}
                          .first_sequence(1)
                          .count(1)
                          .declared_block(500, "short")
                          .seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  const auto only = cursor.next();
  CHECK_FALSE(only.has_value());
  CHECK(only.error_code() == dfr::error::block_overruns_datagram);
}

TEST_CASE("an understated count leaves unconsumed payload",
          "[wire][iextp][regression]") {
  const auto packet =
      raw_packet{}.first_sequence(1).count(1).block("aa").block("bb").seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const iex::message&) { ++delivered; });
  CHECK(delivered == 1);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("verify_payload_framing is the third chain", "[wire][iextp][oracle]") {
  const auto good =
      raw_packet{}.first_sequence(1).count(2).block("aa").block("bb").seal();
  CHECK(iex::verify_payload_framing(good.view()).has_value());

  const auto bad =
      raw_packet{}.first_sequence(1).count(1).block("aa").block("bb").seal();
  const auto outcome = iex::verify_payload_framing(bad.view());
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("next() on an exhausted cursor is a programmer error",
          "[wire][iextp]") {
  DFR_CHECK_ABORTS({
    const auto packet = raw_packet{}.count(0).seal();
    iex::message_cursor cursor;
    static_cast<void>(iex::message_cursor::over(packet.view()).get(cursor));
    static_cast<void>(cursor.next());
  });
}

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

#include <dfr/core/packet_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

// 0x00 .. 0x0F, so that a read's value identifies the offset it came from.
constexpr std::array<std::uint8_t, 16> kRamp = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

dfr::packet_view ramp() { return {kRamp.data(), kRamp.size()}; }

}  // namespace

TEST_CASE("a default view is empty and null", "[core][packet_view]") {
  const dfr::packet_view v;

  CHECK(v.empty());
  CHECK(v.size() == 0);
  CHECK(v.data() == nullptr);
  // A zero-length view must accept a zero-length query rather than rejecting
  // it, so that a caller draining a buffer does not need a special case.
  CHECK(v.contains(0, 0));
  CHECK_FALSE(v.contains(0, 1));
}

TEST_CASE("a view constructs from every byte spelling", "[core][packet_view]") {
  // Capture libraries hand back std::byte, char, unsigned char and uint8_t
  // depending on which one wrote them. Requiring the caller to cast is friction
  // that invites the wrong cast.
  const std::array<char, 4> chars{'a', 'b', 'c', 'd'};
  const std::array<unsigned char, 4> uchars{1, 2, 3, 4};
  const std::array<std::byte, 4> bytes{std::byte{9}, std::byte{8}, std::byte{7},
                                       std::byte{6}};

  CHECK(dfr::packet_view{chars.data(), chars.size()}.size() == 4);
  CHECK(dfr::packet_view{uchars.data(), uchars.size()}.u8_at(0) == 1);
  CHECK(dfr::packet_view{bytes.data(), bytes.size()}.u8_at(0) == 9);

  const std::vector<std::uint8_t> owned{5, 6, 7};
  const dfr::packet_view from_span{std::span{owned}};
  CHECK(from_span.size() == 3);
  CHECK(from_span.u8_at(2) == 7);
}

TEST_CASE("contains() cannot be defeated by integer overflow",
          "[core][packet_view]") {
  const dfr::packet_view v = ramp();
  constexpr std::size_t max = std::numeric_limits<std::size_t>::max();

  // The naive check `offset + length <= size()` wraps for these and returns
  // true, which is exactly how a bounds check gets bypassed by a hostile
  // length field. The implementation avoids the addition entirely.
  CHECK_FALSE(v.contains(max, 10));
  CHECK_FALSE(v.contains(10, max));
  CHECK_FALSE(v.contains(max, max));
  CHECK_FALSE(v.contains(max / 2, max / 2));

  // And it still accepts the exact-fit and empty-at-the-end cases.
  CHECK(v.contains(0, 16));
  CHECK(v.contains(16, 0));
  CHECK(v.contains(15, 1));
  CHECK_FALSE(v.contains(16, 1));
  CHECK_FALSE(v.contains(0, 17));
}

TEST_CASE("subview cannot escape its parent", "[core][packet_view]") {
  const dfr::packet_view v = ramp();

  dfr::packet_view inner;
  REQUIRE(v.subview(4, 8).get(inner) == dfr::error::ok);
  CHECK(inner.size() == 8);
  CHECK(inner.u8_at(0) == 0x04);
  CHECK(inner.u8_at(7) == 0x0B);
  CHECK(inner.end() <= v.end());

  // A nested slice is bounded by the inner view, not the original buffer, even
  // though the bytes are there.
  CHECK_FALSE(inner.subview(0, 9).has_value());
  CHECK(inner.subview(0, 8).has_value());
}

TEST_CASE("an out-of-range slice reports a framing error, not a usage error",
          "[core][packet_view]") {
  const dfr::packet_view v = ramp();
  const auto sliced = v.subview(10, 10);

  CHECK_FALSE(sliced.has_value());
  // The distinction matters. Reaching here means the *wire data* asked for more
  // bytes than arrived, which is this library's product. Classifying it as
  // invalid_argument would file a real protocol observation under programmer
  // error.
  CHECK(sliced.error_code() == dfr::error::block_overruns_datagram);
  CHECK(dfr::is_framing_error(sliced.error_code()));
  CHECK_FALSE(sliced.is_fatal());
}

TEST_CASE("prefix, suffix and consume walk a buffer", "[core][packet_view]") {
  dfr::packet_view v = ramp();

  dfr::packet_view head;
  REQUIRE(v.prefix(4).get(head) == dfr::error::ok);
  CHECK(head.size() == 4);
  CHECK(head.u8_at(3) == 0x03);

  dfr::packet_view tail;
  REQUIRE(v.suffix(12).get(tail) == dfr::error::ok);
  CHECK(tail.size() == 4);
  CHECK(tail.u8_at(0) == 0x0C);

  REQUIRE(v.consume(6).has_value());
  CHECK(v.size() == 10);
  CHECK(v.u8_at(0) == 0x06);

  // Consuming exactly the remainder is legal and leaves an empty view.
  REQUIRE(v.consume(10).has_value());
  CHECK(v.empty());
  CHECK_FALSE(v.consume(1).has_value());

  // suffix at exactly size() is the empty view, not an error.
  CHECK(ramp().suffix(16).has_value());
  CHECK_FALSE(ramp().suffix(17).has_value());
}

TEST_CASE("integer reads honour the endianness in their name",
          "[core][packet_view]") {
  const dfr::packet_view v = ramp();

  CHECK(v.u8_at(1) == 0x01);

  CHECK(v.be16_at(0) == 0x0001);
  CHECK(v.le16_at(0) == 0x0100);

  CHECK(v.be32_at(0) == 0x0001'0203);
  CHECK(v.le32_at(0) == 0x0302'0100);

  CHECK(v.be64_at(0) == 0x0001'0203'0405'0607ULL);
  CHECK(v.le64_at(0) == 0x0706'0504'0302'0100ULL);

  // At a non-zero offset, so an implementation that ignored the offset would
  // still fail.
  CHECK(v.be32_at(4) == 0x0405'0607);
  CHECK(v.le32_at(4) == 0x0706'0504);
}

TEST_CASE("be48_at reads an ITCH timestamp", "[core][packet_view]") {
  // ITCH 5.0 timestamps are exactly six bytes, nanoseconds since midnight.
  // Reaching for be64_at here would read two bytes of the following field,
  // which is the mistake this accessor exists to prevent.
  const dfr::packet_view v = ramp();

  CHECK(v.be48_at(0) == 0x0001'0203'0405ULL);
  CHECK(v.be48_at(2) == 0x0203'0405'0607ULL);

  // The top 16 bits of the result must always be zero: the value is 48 bits.
  CHECK((v.be48_at(0) >> 48) == 0);
}

TEST_CASE("read_at copies a trivially copyable struct",
          "[core][packet_view]") {
  struct wire_pair {
    std::uint32_t first;
    std::uint32_t second;
  };

  const dfr::packet_view v = ramp();
  const auto pair = v.read_at<wire_pair>(0);

  // The contract is byte-for-byte fidelity, not interpretation: read_at gets
  // the bytes out safely and the caller converts. Assert exactly that, by
  // comparing the struct's storage against the source bytes.
  CHECK(std::memcmp(&pair, kRamp.data(), sizeof(pair)) == 0);

  // The point of memcpy over reinterpret_cast: this must work at an offset with
  // no alignment guarantee. A cast to uint32_t* at offset 1 is undefined
  // behaviour that happens to work on x86 and can trap elsewhere.
  const auto unaligned = v.read_at<wire_pair>(1);
  CHECK(std::memcmp(&unaligned, kRamp.data() + 1, sizeof(unaligned)) == 0);
}

TEST_CASE("views compare by content, not by address",
          "[core][packet_view]") {
  const std::array<std::uint8_t, 4> a{1, 2, 3, 4};
  const std::array<std::uint8_t, 4> b{1, 2, 3, 4};
  const std::array<std::uint8_t, 4> c{1, 2, 3, 5};

  CHECK(dfr::packet_view{a.data(), a.size()} ==
        dfr::packet_view{b.data(), b.size()});
  CHECK_FALSE(dfr::packet_view{a.data(), a.size()} ==
              dfr::packet_view{c.data(), c.size()});
  CHECK_FALSE(dfr::packet_view{a.data(), a.size()} ==
              dfr::packet_view{a.data(), 3});

  // Two empty views are equal regardless of where they point.
  CHECK(dfr::packet_view{} == dfr::packet_view{a.data(), 0});
}

TEST_CASE("the accessors are usable at compile time",
          "[core][packet_view]") {
  // constexpr matters beyond elegance: it lets a wire-format test build its
  // expected values with the same code the decoder uses, without a runtime
  // fixture.
  static constexpr std::array<std::byte, 4> bytes{
      std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  static constexpr dfr::packet_view v{bytes.data(), bytes.size()};

  STATIC_REQUIRE(v.size() == 4);
  STATIC_REQUIRE(v.contains(0, 4));
  STATIC_REQUIRE(v.be32_at(0) == 0xDEAD'BEEF);
  STATIC_REQUIRE(v.le16_at(0) == 0xADDE);
}

// ---------------------------------------------------------------------------
// mutable_packet_view
// ---------------------------------------------------------------------------

TEST_CASE("writes round-trip through the matching reader",
          "[core][packet_view]") {
  std::array<std::uint8_t, 16> buffer{};
  const dfr::mutable_packet_view w{buffer.data(), buffer.size()};
  const dfr::packet_view r = w;

  w.put_u8_at(0, 0xA5);
  CHECK(r.u8_at(0) == 0xA5);

  w.put_be16_at(2, 0x1234);
  CHECK(r.be16_at(2) == 0x1234);
  CHECK(r.u8_at(2) == 0x12);  // big-endian: most significant byte first
  CHECK(r.u8_at(3) == 0x34);

  w.put_le16_at(4, 0x1234);
  CHECK(r.le16_at(4) == 0x1234);
  CHECK(r.u8_at(4) == 0x34);  // little-endian: least significant byte first
  CHECK(r.u8_at(5) == 0x12);

  w.put_be32_at(6, 0xDEAD'BEEF);
  CHECK(r.be32_at(6) == 0xDEAD'BEEF);

  w.put_be64_at(8, 0x0102'0304'0506'0708ULL);
  CHECK(r.be64_at(8) == 0x0102'0304'0506'0708ULL);
  w.put_le64_at(8, 0x0102'0304'0506'0708ULL);
  CHECK(r.le64_at(8) == 0x0102'0304'0506'0708ULL);
}

TEST_CASE("flip_bit_at corrupts exactly one bit", "[core][packet_view]") {
  // Single-bit corruption is the fault class TigerBeetle's simulator missed for
  // years because it only ever corrupted whole sectors. dfr::chaos needs it, so
  // the primitive lives here.
  std::array<std::uint8_t, 2> buffer{0x00, 0xFF};
  const dfr::mutable_packet_view w{buffer.data(), buffer.size()};
  const dfr::packet_view r = w;

  w.flip_bit_at(0, 3);
  CHECK(r.u8_at(0) == 0x08);
  CHECK(r.u8_at(1) == 0xFF);  // the neighbour is untouched

  w.flip_bit_at(0, 3);
  CHECK(r.u8_at(0) == 0x00);  // flipping twice restores

  w.flip_bit_at(1, 7);
  CHECK(r.u8_at(1) == 0x7F);
}

TEST_CASE("a mutable view narrows to a const one but not back",
          "[core][packet_view]") {
  std::array<std::uint8_t, 4> buffer{1, 2, 3, 4};
  const dfr::mutable_packet_view w{buffer.data(), buffer.size()};

  const dfr::packet_view implicit = w;   // implicit narrowing
  const dfr::packet_view named = w.as_const();

  CHECK(implicit == named);

  // The reverse must not compile. Asserted as a type property rather than as a
  // commented-out line, so it is actually checked.
  STATIC_REQUIRE(std::is_convertible_v<dfr::mutable_packet_view,
                                       dfr::packet_view>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<dfr::packet_view,
                                             dfr::mutable_packet_view>);

  // And obtaining a writable window is explicit, per Abseil's rule that a
  // const view converts implicitly while a mutable one does not.
  STATIC_REQUIRE_FALSE(
      std::is_convertible_v<std::span<std::byte>, dfr::mutable_packet_view>);
  STATIC_REQUIRE(std::is_constructible_v<dfr::mutable_packet_view,
                                         std::span<std::byte>>);
}

TEST_CASE("a mutable subview is bounded like a const one",
          "[core][packet_view]") {
  std::array<std::uint8_t, 8> buffer{};
  const dfr::mutable_packet_view w{buffer.data(), buffer.size()};

  dfr::mutable_packet_view inner;
  REQUIRE(w.subview(4, 4).get(inner) == dfr::error::ok);
  CHECK(inner.size() == 4);
  CHECK_FALSE(w.subview(4, 5).has_value());

  inner.put_u8_at(0, 0x77);
  CHECK(buffer[4] == 0x77);
}

// ---------------------------------------------------------------------------
// The defect this header exists to prevent
// ---------------------------------------------------------------------------

TEST_CASE("walking a block sequence catches an overstated block count",
          "[core][packet_view][regression]") {
  // This reproduces the shape of the penberg/helix defect
  // (moldudp64.hh:106-115): a header declares a block count, and the loop
  // trusts it. Here the datagram carries a 2-byte count of 4, then only two
  // 1-byte blocks — each prefixed by its own 2-byte length, as MoldUDP64 does.
  //
  // A decoder that trusts the count reads past the end. One that slices through
  // subview() gets an error on the third iteration instead.
  const std::array<std::uint8_t, 8> datagram{
      0x00, 0x04,        // block count = 4 (a lie)
      0x00, 0x01, 0xAA,  // block 1: length 1, payload 0xAA
      0x00, 0x01, 0xBB,  // block 2: length 1, payload 0xBB
  };

  dfr::packet_view cursor{datagram.data(), datagram.size()};
  const std::uint16_t declared_blocks = cursor.be16_at(0);
  REQUIRE(declared_blocks == 4);
  REQUIRE(cursor.consume(2).has_value());

  std::vector<std::uint8_t> payloads;
  dfr::error stopped_with = dfr::error::ok;

  for (std::uint16_t i = 0; i < declared_blocks; ++i) {
    dfr::packet_view length_field;
    if (const auto err = cursor.prefix(2).get(length_field);
        err != dfr::error::ok) {
      stopped_with = err;
      break;
    }
    const std::uint16_t length = length_field.be16_at(0);
    if (const auto err = cursor.consume(2); !err) {
      stopped_with = err.error_code();
      break;
    }

    dfr::packet_view block;
    if (const auto err = cursor.prefix(length).get(block);
        err != dfr::error::ok) {
      stopped_with = err;
      break;
    }
    payloads.push_back(block.u8_at(0));
    if (const auto err = cursor.consume(length); !err) {
      stopped_with = err.error_code();
      break;
    }
  }

  // Two real blocks were decoded, and the walk stopped at the boundary rather
  // than reading whatever followed the buffer in memory.
  CHECK(payloads.size() == 2);
  CHECK(payloads[0] == 0xAA);
  CHECK(payloads[1] == 0xBB);
  CHECK(stopped_with == dfr::error::block_overruns_datagram);
  CHECK(cursor.empty());
}

TEST_CASE("a block whose length field overruns the datagram is rejected",
          "[core][packet_view][regression]") {
  // The other half of the same defect: the count is honest but a block's own
  // length field is not.
  const std::array<std::uint8_t, 5> datagram{
      0x00, 0x01,  // block count = 1
      0xFF, 0xFF,  // block length = 65535
      0xAA,        // one byte of payload
  };

  dfr::packet_view cursor{datagram.data(), datagram.size()};
  REQUIRE(cursor.consume(2).has_value());

  const std::uint16_t length = cursor.be16_at(0);
  REQUIRE(length == 0xFFFF);
  REQUIRE(cursor.consume(2).has_value());

  const auto block = cursor.prefix(length);
  CHECK_FALSE(block.has_value());
  CHECK(block.error_code() == dfr::error::block_overruns_datagram);
}

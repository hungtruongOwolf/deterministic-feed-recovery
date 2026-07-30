#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

}  // namespace


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

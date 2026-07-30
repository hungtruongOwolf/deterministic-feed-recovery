#include <dfr/core/byte_order.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("byteswap reverses each width", "[core][byte_order]") {
  STATIC_REQUIRE(dfr::byteswap(std::uint8_t{0xAB}) == 0xAB);
  STATIC_REQUIRE(dfr::byteswap(std::uint16_t{0x1234}) == 0x3412);
  STATIC_REQUIRE(dfr::byteswap(std::uint32_t{0x1234'5678}) == 0x7856'3412);
  STATIC_REQUIRE(dfr::byteswap(std::uint64_t{0x0123'4567'89AB'CDEF}) ==
                 0xEFCD'AB89'6745'2301);
}

TEST_CASE("byteswap is its own inverse", "[core][byte_order]") {
  // Includes the boundary values, because a shift-and-mask implementation that
  // drops the top byte still passes on a value with a zero there.
  const std::uint64_t values[] = {
      0, 1, 0xFF, 0xFF00, std::numeric_limits<std::uint64_t>::max(),
      0x8000'0000'0000'0000ULL, 0x0123'4567'89AB'CDEFULL};

  for (const std::uint64_t v : values) {
    CHECK(dfr::byteswap(dfr::byteswap(v)) == v);
    const auto v32 = static_cast<std::uint32_t>(v);
    CHECK(dfr::byteswap(dfr::byteswap(v32)) == v32);
    const auto v16 = static_cast<std::uint16_t>(v);
    CHECK(dfr::byteswap(dfr::byteswap(v16)) == v16);
  }
}

TEST_CASE("byteswap preserves the top byte", "[core][byte_order]") {
  // A masking bug that loses the high byte is the classic hand-rolled-byteswap
  // defect, and it is invisible unless the input has a non-zero there.
  CHECK(dfr::byteswap(std::uint16_t{0xFF00}) == 0x00FF);
  CHECK(dfr::byteswap(std::uint32_t{0xFF00'0000}) == 0x0000'00FF);
  CHECK(dfr::byteswap(std::uint64_t{0xFF00'0000'0000'0000ULL}) ==
        0x0000'0000'0000'00FFULL);
}

TEST_CASE("the endian conversions are their own inverses",
          "[core][byte_order]") {
  const std::uint32_t host = 0xDEAD'BEEF;

  CHECK(dfr::from_big_endian(dfr::to_big_endian(host)) == host);
  CHECK(dfr::from_little_endian(dfr::to_little_endian(host)) == host);
}

TEST_CASE("the conversions agree with a known byte layout",
          "[core][byte_order]") {
  // Pinning against an explicit byte pattern rather than against the platform,
  // so this test would fail on a big-endian machine if the direction were
  // reversed — which a round-trip test alone cannot catch.
  const std::uint32_t host = 0x0102'0304;

  const std::uint32_t big = dfr::to_big_endian(host);
  const auto* big_bytes = reinterpret_cast<const unsigned char*>(&big);
  CHECK(big_bytes[0] == 0x01);
  CHECK(big_bytes[1] == 0x02);
  CHECK(big_bytes[2] == 0x03);
  CHECK(big_bytes[3] == 0x04);

  const std::uint32_t little = dfr::to_little_endian(host);
  const auto* little_bytes = reinterpret_cast<const unsigned char*>(&little);
  CHECK(little_bytes[0] == 0x04);
  CHECK(little_bytes[1] == 0x03);
  CHECK(little_bytes[2] == 0x02);
  CHECK(little_bytes[3] == 0x01);
}

TEST_CASE("only the non-native path swaps", "[core][byte_order]") {
  // CHECK rather than STATIC_REQUIRE, deliberately. `if constexpr` discards the
  // untaken branch only inside a template; in a plain function both branches
  // are still parsed and their static_asserts still evaluated, so a
  // STATIC_REQUIRE here would fail on every platform. The values are still
  // computed at compile time — only the assertion is deferred.
  constexpr std::uint32_t host = 0x1234'5678;
  constexpr std::uint32_t swapped = 0x7856'3412;

  if constexpr (dfr::kNativeIsLittleEndian) {
    CHECK(dfr::from_little_endian(host) == host);
    CHECK(dfr::from_big_endian(host) == swapped);
  } else {
    CHECK(dfr::from_big_endian(host) == host);
    CHECK(dfr::from_little_endian(host) == swapped);
  }
}

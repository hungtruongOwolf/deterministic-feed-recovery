// Fixed-width ASCII fields: two padding conventions, and a reachable overflow.

#include <dfr/wire/soupbintcp/ascii.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace soup = dfr::wire::soupbintcp;

namespace {

dfr::packet_view field_of(std::string_view text) {
  return dfr::packet_view{text.data(), text.size()};
}

std::string trimmed(std::string_view text) {
  std::string_view out;
  REQUIRE(soup::text_left_justified(field_of(text)).get(out) == dfr::error::ok);
  return std::string{out};
}

dfr::result<std::uint64_t> number(std::string_view text) {
  return soup::number_right_justified(field_of(text));
}

}  // namespace

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

TEST_CASE("a left-justified field drops its trailing padding",
          "[wire][soupbintcp]") {
  CHECK(trimmed("TEST      ") == "TEST");
  CHECK(trimmed("ABCDEFGHIJ") == "ABCDEFGHIJ");
  CHECK(trimmed("          ").empty());
  CHECK(trimmed("A B       ") == "A B");  // an interior space is content
}

TEST_CASE("a left-justified field keeps leading spaces",
          "[wire][soupbintcp][regression]") {
  // Trimming both ends would silently accept a right-justified field and hide the mistake. A session
  // id read from the wrong convention compares unequal to itself, which presents as a login that is
  // rejected for no visible reason.
  CHECK(trimmed("   TEST   ") == "   TEST");
}

TEST_CASE("a right-justified number drops its leading padding",
          "[wire][soupbintcp]") {
  std::uint64_t value = 0;
  REQUIRE(number("                  42").get(value) == dfr::error::ok);
  CHECK(value == 42);
  REQUIRE(number("00000000000000000042").get(value) == dfr::error::ok);
  CHECK(value == 42);
  REQUIRE(number("                   0").get(value) == dfr::error::ok);
  CHECK(value == 0);
}

TEST_CASE("a right-justified field rejects the other convention",
          "[wire][soupbintcp][regression]") {
  // "42" followed by spaces is the left-justified layout. Read as a number it would either be 42 —
  // by ignoring the padding — or something enormous, depending on how the digits were consumed.
  // Neither is right, and the difference is a factor of ten per pad byte.
  CHECK(number("42                  ").error_code() == dfr::error::invalid_argument);
}

TEST_CASE("an all-space numeric field is not zero", "[wire][soupbintcp]") {
  // In a Login Request, sequence zero means "start me at the next message you send". An unset field
  // read as zero would turn a caller's omission into a deliberate instruction.
  CHECK(number("                    ").error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a non-digit is rejected", "[wire][soupbintcp]") {
  CHECK(number("                 4x2").error_code() == dfr::error::invalid_argument);
  CHECK(number("                 -42").error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a twenty-digit overflow is reported, not wrapped",
          "[wire][soupbintcp][regression]") {
  // Reachable, not theoretical: twenty digits can express 99,999,999,999,999,999,999 and a 64-bit
  // unsigned integer stops at 18,446,744,073,709,551,615 — also twenty digits. A wrapped value would
  // be a small, believable sequence number, which is the worst possible way for this to fail.
  std::uint64_t value = 0;
  REQUIRE(number("18446744073709551615").get(value) == dfr::error::ok);
  CHECK(value == UINT64_MAX);

  CHECK(number("18446744073709551616").error_code() == dfr::error::invalid_argument);
  CHECK(number("99999999999999999999").error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

TEST_CASE("text is written left-justified and padded right",
          "[wire][soupbintcp]") {
  std::array<std::byte, 10> buffer{};
  const dfr::mutable_packet_view field{buffer.data(), buffer.size()};
  REQUIRE(soup::put_text_left_justified(field, "TEST").has_value());
  CHECK(trimmed(std::string{reinterpret_cast<const char*>(buffer.data()), 10}) ==
        "TEST");
  CHECK(buffer[4] == std::byte{' '});
  CHECK(buffer[9] == std::byte{' '});
}

TEST_CASE("a number is written right-justified and padded left",
          "[wire][soupbintcp]") {
  std::array<std::byte, 20> buffer{};
  const dfr::mutable_packet_view field{buffer.data(), buffer.size()};
  REQUIRE(soup::put_number_right_justified(field, 42).has_value());
  CHECK(buffer[0] == std::byte{' '});
  CHECK(buffer[18] == std::byte{'4'});
  CHECK(buffer[19] == std::byte{'2'});
}

TEST_CASE("zero is written as a single digit, not as padding",
          "[wire][soupbintcp]") {
  // An all-space field means "unset" and zero means "start at the next message". Writing zero as
  // twenty spaces would send the first as if it were the second.
  std::array<std::byte, 20> buffer{};
  const dfr::mutable_packet_view field{buffer.data(), buffer.size()};
  REQUIRE(soup::put_number_right_justified(field, 0).has_value());
  CHECK(buffer[19] == std::byte{'0'});
  CHECK(buffer[18] == std::byte{' '});

  std::uint64_t read_back = 0;
  REQUIRE(soup::number_right_justified(
              dfr::packet_view{buffer.data(), buffer.size()})
              .get(read_back) == dfr::error::ok);
  CHECK(read_back == 0);
}

TEST_CASE("content too long for the field is refused, not truncated",
          "[wire][soupbintcp]") {
  // A silently shortened session id or username produces a login rejection whose cause is invisible
  // in the packet that was sent.
  std::array<std::byte, 6> buffer{};
  const dfr::mutable_packet_view field{buffer.data(), buffer.size()};
  CHECK(soup::put_text_left_justified(field, "TOOLONGNAME").error_code() ==
        dfr::error::invalid_argument);
  CHECK(soup::put_number_right_justified(field, 1'234'567).error_code() ==
        dfr::error::invalid_argument);
}

TEST_CASE("every twenty-digit number round-trips", "[wire][soupbintcp]") {
  // The encoder and the decoder agree across the whole representable range, including the boundary
  // where one more digit would overflow.
  std::array<std::byte, 20> buffer{};
  const dfr::mutable_packet_view field{buffer.data(), buffer.size()};
  for (const std::uint64_t value :
       {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{9},
        std::uint64_t{10}, std::uint64_t{999'999'999},
        std::uint64_t{1'000'000'000'000'000'000}, UINT64_MAX}) {
    REQUIRE(soup::put_number_right_justified(field, value).has_value());
    std::uint64_t read_back = 0;
    REQUIRE(soup::number_right_justified(
                dfr::packet_view{buffer.data(), buffer.size()})
                .get(read_back) == dfr::error::ok);
    CHECK(read_back == value);
  }
}

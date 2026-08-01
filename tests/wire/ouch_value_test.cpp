// The two OUCH value types that carry traps: price and token.

#include <dfr/wire/ouch/price.hpp>
#include <dfr/wire/ouch/token.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace ouch = dfr::wire::ouch;

// ---------------------------------------------------------------------------
// Price
// ---------------------------------------------------------------------------

TEST_CASE("a price is ten-thousandths of a dollar", "[wire][ouch]") {
  ouch::price p;
  REQUIRE(ouch::price::from_dollars_and_ten_thousandths(12, 3'400).get(p) ==
          dfr::error::ok);
  CHECK(p.raw() == 123'400);
  CHECK(p.dollars() == 12);
  CHECK(p.ten_thousandths() == 3'400);
  CHECK_FALSE(p.is_market());
}

TEST_CASE("market orders live at the top of the price field",
          "[wire][ouch][regression]") {
  // The specification names $214,748.3647 (0x7FFFFFFF) as the market price, and then adds that "orders
  // entered with a price of $200,000.00 or the max integer value will also be treated as market
  // orders". So the boundary is a threshold, not a sentinel.
  //
  // An implementation comparing only against 0x7FFFFFFF would send a $250,000 limit order and have it
  // executed at any price. That is a money bug hiding inside an equality test.
  CHECK(ouch::price::market().is_market());
  CHECK(ouch::price::from_raw(0x7FFF'FFFFU).is_market());
  CHECK(ouch::price::from_raw(ouch::kMarketThresholdRaw).is_market());  // $200,000.00
  CHECK(ouch::price::from_raw(2'500'000'000U).is_market());             // $250,000.00
  CHECK(ouch::price::from_raw(UINT32_MAX).is_market());

  // And the highest value that is still a limit is not.
  CHECK_FALSE(ouch::price::from_raw(ouch::kMaxLimitRaw).is_market());
  CHECK(ouch::price::from_raw(ouch::kMaxLimitRaw).is_valid_limit());
}

TEST_CASE("the specification's own hex value pins the limit maximum",
          "[wire][ouch]") {
  // §1.2 states it as 7735939C hex, so the decimal and the hex are checked against each other rather
  // than one being trusted.
  STATIC_REQUIRE(ouch::kMaxLimitRaw == 0x7735'939CU);
  CHECK(ouch::kMaxLimitRaw == 1'999'999'900U);
}

TEST_CASE("a price above the limit maximum cannot be built by accident",
          "[wire][ouch]") {
  CHECK(ouch::price::from_dollars_and_ten_thousandths(200'000, 0).error_code() ==
        dfr::error::invalid_argument);
  // Six whole places, so a million dollars is out of range, and it is refused before the
  // multiplication, which would otherwise wrap into a plausible small price.
  CHECK(ouch::price::from_dollars_and_ten_thousandths(1'000'000, 0).error_code() ==
        dfr::error::invalid_argument);
  CHECK(ouch::price::from_dollars_and_ten_thousandths(12, 10'000).error_code() ==
        dfr::error::invalid_argument);
}

TEST_CASE("the band between the limit maximum and market is neither",
          "[wire][ouch]") {
  // The specification leaves it undefined, so it is reported rather than guessed at.
  const auto odd = ouch::price::from_raw(ouch::kMaxLimitRaw + 1);
  CHECK_FALSE(odd.is_market());
  CHECK_FALSE(odd.is_valid_limit());
}

TEST_CASE("asking a market order for its dollar price aborts",
          "[wire][ouch]") {
  // Printing $214,748.36 for a market order would be worse than refusing: it is a number that looks
  // like a price and is not one.
  DFR_CHECK_ABORTS((void)ouch::price::market().dollars());
}

// ---------------------------------------------------------------------------
// Token
// ---------------------------------------------------------------------------

TEST_CASE("a token compares over all fourteen bytes", "[wire][ouch]") {
  // The field is fixed-width and space-padded, so "ABC" and "ABC" plus eleven spaces are the same token
  // and must compare equal. This is what makes the protocol's re-send-anything failover model safe.
  ouch::order_token from_text;
  REQUIRE(ouch::order_token::from_text("ABC").get(from_text) == dfr::error::ok);

  const std::string padded = "ABC           ";
  REQUIRE(padded.size() == 14);
  ouch::order_token from_wire;
  REQUIRE(ouch::order_token::from_bytes(
              dfr::packet_view{padded.data(), padded.size()})
              .get(from_wire) == dfr::error::ok);

  CHECK(from_text == from_wire);
  CHECK(from_text.text() == "ABC");
}

TEST_CASE("tokens are case sensitive", "[wire][ouch]") {
  // §1.2 says so outright. Folding case would merge two orders the client considers distinct, and the
  // second would be silently ignored as a duplicate.
  ouch::order_token lower;
  ouch::order_token upper;
  REQUIRE(ouch::order_token::from_text("abc").get(lower) == dfr::error::ok);
  REQUIRE(ouch::order_token::from_text("ABC").get(upper) == dfr::error::ok);
  CHECK_FALSE(lower == upper);
}

TEST_CASE("a default token is spaces, not zeros", "[wire][ouch]") {
  // Fourteen NUL bytes would be a different token rather than an absent one, and the exchange would
  // accept it as a real identifier.
  const ouch::order_token blank;
  CHECK(blank.empty());
  CHECK(blank.bytes().u8_at(0) == ' ');
  CHECK(blank.bytes().u8_at(13) == ' ');
}

TEST_CASE("spaces inside a token are content", "[wire][ouch]") {
  // §1.2 allows spaces, so only the trailing run is padding.
  ouch::order_token spaced;
  REQUIRE(ouch::order_token::from_text("A B").get(spaced) == dfr::error::ok);
  CHECK(spaced.text() == "A B");
}

TEST_CASE("a token too long is refused, not truncated",
          "[wire][ouch][regression]") {
  // Two orders whose tokens differ only past the fourteenth character would silently become one, and
  // the second would be ignored as a duplicate: losing an order rather than reporting a mistake.
  CHECK(ouch::order_token::from_text("012345678901234").error_code() ==
        dfr::error::invalid_argument);
  REQUIRE(ouch::order_token::from_text("01234567890123").has_value());
}

TEST_CASE("a token field of the wrong width is refused", "[wire][ouch]") {
  const std::string short_field = "ABC";
  CHECK(ouch::order_token::from_bytes(
            dfr::packet_view{short_field.data(), short_field.size()})
            .error_code() == dfr::error::message_length_mismatch);
}

TEST_CASE("a token round-trips through a buffer", "[wire][ouch]") {
  std::array<std::byte, 14> buffer{};
  ouch::order_token original;
  REQUIRE(ouch::order_token::from_text("ORD-000042").get(original) ==
          dfr::error::ok);
  original.write_to(dfr::mutable_packet_view{buffer.data(), buffer.size()});

  ouch::order_token read_back;
  REQUIRE(ouch::order_token::from_bytes(
              dfr::packet_view{buffer.data(), buffer.size()})
              .get(read_back) == dfr::error::ok);
  CHECK(read_back == original);
  CHECK(read_back.text() == "ORD-000042");
}

TEST_CASE("tokens order so a registry can binary-search them",
          "[wire][ouch]") {
  ouch::order_token a;
  ouch::order_token b;
  REQUIRE(ouch::order_token::from_text("AAA").get(a) == dfr::error::ok);
  REQUIRE(ouch::order_token::from_text("AAB").get(b) == dfr::error::ok);
  CHECK(a < b);
  CHECK_FALSE(b < a);
}

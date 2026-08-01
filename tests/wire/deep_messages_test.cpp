// The DEEP decoders, checked against bytes taken out of a real capture.
//
// The samples below are not constructed. Each is the first message of its type in the 2017-08-26 IEX HIST DEEP
// file, copied byte for byte. That matters more than the number of assertions: a decoder tested only against
// bytes its own encoder produced agrees with itself about a layout that may be wrong, which is the failure
// docs/DESIGN.md describes and the reason this library verifies against captures.
//
// The strongest assertion here is not any single field. It is that a Price Level Update Buy and a Sell for the
// same symbol at the same instant decode to $20.8900 and $20.9000: a valid one-cent spread. A wrong price
// offset cannot produce a coherent spread by accident.

#include <dfr/wire/deep.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace deep = dfr::wire::deep;

namespace {

// The first Price Level Update Buy in the capture: WWE, 100 shares, $20.8900.
constexpr std::array<std::uint8_t, 30> kBuy{0x38, 0x01, 0xb3, 0x01, 0x67, 0x59, 0xe4, 0x63,
                                            0xde, 0x14, 0x57, 0x57, 0x45, 0x20, 0x20, 0x20,
                                            0x20, 0x20, 0x64, 0x00, 0x00, 0x00, 0x04, 0x30,
                                            0x03, 0x00, 0x00, 0x00, 0x00, 0x00};

// The first Sell, same symbol, same microsecond: $20.9000.
constexpr std::array<std::uint8_t, 30> kSell{0x35, 0x01, 0xda, 0xba, 0x69, 0x59, 0xe4, 0x63,
                                             0xde, 0x14, 0x57, 0x57, 0x45, 0x20, 0x20, 0x20,
                                             0x20, 0x20, 0x64, 0x00, 0x00, 0x00, 0x68, 0x30,
                                             0x03, 0x00, 0x00, 0x00, 0x00, 0x00};

// The first Trade Report: WWE, 100 shares at $20.9000, trade id 281,637.
constexpr std::array<std::uint8_t, 38> kTrade{
    0x54, 0xc0, 0x18, 0x54, 0x0c, 0x3f, 0x3f, 0x64, 0xde, 0x14, 0x57, 0x57, 0x45,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x64, 0x00, 0x00, 0x00, 0x68, 0x30, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x25, 0x4c, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};

// The first System Event: 'O', start of messages.
constexpr std::array<std::uint8_t, 10> kSystem{0x53, 0x4f, 0x70, 0x27, 0x92,
                                               0x55, 0x7d, 0x61, 0xde, 0x14};

// The first Trading Status: symbol "A", status 'T' for trading.
constexpr std::array<std::uint8_t, 22> kStatus{0x48, 0x54, 0x70, 0x27, 0x92, 0x55, 0x7d, 0x61,
                                               0xde, 0x14, 0x41, 0x20, 0x20, 0x20, 0x20, 0x20,
                                               0x20, 0x20, 0x20, 0x20, 0x20, 0x20};

template <std::size_t N>
dfr::packet_view view(const std::array<std::uint8_t, N>& bytes) {
  return dfr::packet_view{bytes.data(), bytes.size()};
}

// 2017-08-26, in nanoseconds since the epoch. Every timestamp in the capture must land inside that day, which
// is the check that settles the timestamp offset: a wrong offset lands in 1970 or in the far future.
constexpr std::uint64_t kDayStart = 1'503'705'600'000'000'000ULL;  // 2017-08-26 00:00:00 UTC
constexpr std::uint64_t kDayEnd = kDayStart + 86'400'000'000'000ULL;

}  // namespace

TEST_CASE("a real Price Level Update Buy decodes to a real quote") {
  deep::price_level_update out;
  REQUIRE(deep::decode_price_level(view(kBuy)).get(out) == dfr::error::ok);

  CHECK(out.buy);
  CHECK(out.symbol == "WWE");  // trimmed: the wire carries "WWE     "
  CHECK(out.size == 100);
  CHECK(out.level.dollars() == 20);
  CHECK(out.level.ten_thousandths() == 8900);
  CHECK(out.event_complete());
  CHECK(out.head.timestamp_ns > kDayStart);
  CHECK(out.head.timestamp_ns < kDayEnd);
}

TEST_CASE("the buy and the sell make a one-cent spread") {
  deep::price_level_update buy;
  deep::price_level_update sell;
  REQUIRE(deep::decode_price_level(view(kBuy)).get(buy) == dfr::error::ok);
  REQUIRE(deep::decode_price_level(view(kSell)).get(sell) == dfr::error::ok);

  CHECK(buy.symbol == sell.symbol);
  CHECK_FALSE(sell.buy);
  // The assertion that settles the layout. A wrong price offset would give two unrelated numbers; these are one
  // cent apart, in the right order, on the same symbol.
  CHECK(buy.level < sell.level);
  CHECK(sell.level.raw() - buy.level.raw() == 100);  // $0.0100 at four implied decimals
}

TEST_CASE("a real Trade Report decodes, and prints at the ask") {
  deep::trade_report out;
  REQUIRE(deep::decode_trade(view(kTrade)).get(out) == dfr::error::ok);

  CHECK(out.symbol == "WWE");
  CHECK(out.size == 100);
  CHECK(out.at.dollars() == 20);
  CHECK(out.at.ten_thousandths() == 9000);
  CHECK(out.trade_id == 281'637);
  CHECK_FALSE(out.broken);

  deep::price_level_update sell;
  REQUIRE(deep::decode_price_level(view(kSell)).get(sell) == dfr::error::ok);
  // It traded at the offer, which is what an aggressive buy does and is one more piece of evidence that the
  // price offset is right in both messages.
  CHECK(out.at == sell.level);
}

TEST_CASE("a real System Event and Trading Status decode") {
  deep::system_event event;
  REQUIRE(deep::decode_system_event(view(kSystem)).get(event) == dfr::error::ok);
  CHECK(event.event == 'O');
  CHECK(event.head.timestamp_ns > kDayStart);

  deep::trading_status status;
  REQUIRE(deep::decode_trading_status(view(kStatus)).get(status) == dfr::error::ok);
  CHECK(status.symbol == "A");
  CHECK(status.status == 'T');
  CHECK(status.reason.empty());  // four spaces on the wire, trimmed to nothing
}

TEST_CASE("a length that disagrees with the type is refused, never decoded") {
  auto truncated = kBuy;
  const dfr::packet_view short_view{truncated.data(), truncated.size() - 1};
  deep::price_level_update out;
  // Decoding a 29-byte message against the 30-byte layout would read the price one byte short and produce a
  // plausible wrong number. That is worse than refusing, so the length is checked against the type.
  CHECK(deep::decode_price_level(short_view).get(out) == dfr::error::message_length_mismatch);
}

TEST_CASE("an unknown type is named as unknown rather than guessed at") {
  auto unknown = kBuy;
  unknown[0] = 'Q';  // not one of the eleven types the capture contains
  deep::header head;
  CHECK(deep::decode_header(view(unknown)).get(head) == dfr::error::unknown_message_type);
}

TEST_CASE("asking a decoder for the wrong message is refused") {
  deep::trade_report out;
  // A well-formed Price Level Update handed to the trade decoder. Both are valid messages, so only the type
  // check catches this, and a caller dispatching on a type table can get it wrong.
  CHECK(deep::decode_trade(view(kBuy)).get(out) == dfr::error::unknown_message_type);
}

TEST_CASE("every type the capture contains has a size and a name") {
  constexpr std::array kAll{
      deep::message_type::system_event,     deep::message_type::security_directory,
      deep::message_type::trading_status,   deep::message_type::operational_halt,
      deep::message_type::short_sale_test,  deep::message_type::security_event,
      deep::message_type::price_level_buy,  deep::message_type::price_level_sell,
      deep::message_type::trade_report,     deep::message_type::trade_break,
      deep::message_type::auction_information};
  for (const auto type : kAll) {
    CHECK(deep::is_known(static_cast<std::uint8_t>(type)));
    CHECK(deep::expected_size(type) >= 10);
    CHECK_FALSE(deep::name_of(type).empty());
  }
}

TEST_CASE("a negative price decodes as negative rather than enormous") {
  auto negative = kBuy;
  // DEEP uses a negative price for "no price" in some auction fields. An unsigned read would turn an absence
  // into nine quintillion dollars.
  for (std::size_t i = 0; i < 8; ++i) {
    negative[deep::kPriceOffset + i] = 0xFF;
  }
  deep::price_level_update out;
  REQUIRE(deep::decode_price_level(view(negative)).get(out) == dfr::error::ok);
  CHECK(out.level.raw() == -1);
}

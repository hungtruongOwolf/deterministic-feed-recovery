// The book, and the two things about DEEP that a wrong implementation gets wrong.
//
// Aggregated depth has exactly two rules that are easy to state and easy to invert, and inverting either
// produces a book that looks plausible and quotes a price nobody is offering:
//
//   1. a size of zero *removes* the level, it does not set a level to zero shares;
//   2. an update at an existing price *replaces* the size, it does not add to it.
//
// Both are tested here directly, because a test that only checks the happy path would pass on either mistake.

#include <dfr/book/order_book.hpp>

#include <catch2/catch_test_macros.hpp>

namespace book = dfr::book;
namespace deep = dfr::wire::deep;

namespace {

using test_book = book::order_book<8>;

deep::price at(std::int64_t dollars, std::int64_t ten_thousandths = 0) {
  return deep::price{dollars * deep::kPriceScale + ten_thousandths};
}

deep::price_level_update update(bool buy, deep::price level, std::uint32_t size) {
  deep::price_level_update out;
  out.buy = buy;
  out.symbol = "TEST";
  out.level = level;
  out.size = size;
  out.head.type = buy ? deep::message_type::price_level_buy : deep::message_type::price_level_sell;
  return out;
}

}  // namespace

TEST_CASE("an empty book quotes nothing rather than zero") {
  const test_book b;
  CHECK(b.bids().empty());
  CHECK(b.asks().empty());
  CHECK(b.bids().best().size == 0);
  CHECK_FALSE(b.crossed());
}

TEST_CASE("bids sort with the highest first and asks with the lowest") {
  test_book b;
  // Inserted out of order on purpose: the sort is the whole reason the best price is one load away.
  REQUIRE(b.apply(update(true, at(20, 8900), 100)));
  REQUIRE(b.apply(update(true, at(21, 0), 200)));
  REQUIRE(b.apply(update(true, at(20, 9500), 300)));
  REQUIRE(b.apply(update(false, at(21, 500), 400)));
  REQUIRE(b.apply(update(false, at(21, 100), 500)));

  CHECK(b.bids().best().at == at(21, 0));
  CHECK(b.bids().best().size == 200);
  CHECK(b.asks().best().at == at(21, 100));
  CHECK(b.asks().best().size == 500);

  CHECK(b.bids().levels()[1].at == at(20, 9500));
  CHECK(b.bids().levels()[2].at == at(20, 8900));
  CHECK(b.asks().levels()[1].at == at(21, 500));
}

TEST_CASE("a size of zero removes the level rather than emptying it") {
  test_book b;
  REQUIRE(b.apply(update(true, at(20, 8900), 100)));
  REQUIRE(b.apply(update(true, at(20, 9000), 200)));
  REQUIRE(b.bids().size() == 2);

  REQUIRE(b.apply(update(true, at(20, 9000), 0)));

  // The mistake this test exists for: keeping a zero-size level would leave 20.9000 as the best bid forever,
  // and the book would quote a price nobody is offering.
  CHECK(b.bids().size() == 1);
  CHECK(b.bids().best().at == at(20, 8900));
}

TEST_CASE("an update at an existing price replaces the size, never adds to it") {
  test_book b;
  REQUIRE(b.apply(update(false, at(30, 0), 500)));
  REQUIRE(b.apply(update(false, at(30, 0), 200)));

  // DEEP publishes the aggregate at a price. Adding would double the depth on every refresh, which reads as a
  // liquid market that is not there.
  CHECK(b.asks().size() == 1);
  CHECK(b.asks().best().size == 200);
}

TEST_CASE("removing a level that was never there is not an error") {
  test_book b;
  // Legitimate on a feed joined mid-day, and on a book rebuilt from a snapshot that never carried the level the
  // next update deletes. Treating it as an error would make an ordinary morning look like corruption.
  CHECK(b.apply(update(true, at(15, 0), 0)));
  CHECK(b.bids().empty());
}

TEST_CASE("a crossed book is reported rather than repaired") {
  test_book b;
  REQUIRE(b.apply(update(true, at(21, 0), 100)));
  REQUIRE(b.apply(update(false, at(20, 9000), 100)));

  // A DEEP feed publishes one side at a time, so a book is briefly crossed between the halves of a quote
  // change. Silently repairing it would hide the case that matters: one that stays crossed after the event
  // completes.
  CHECK(b.crossed());
  CHECK(b.bids().best().at > b.asks().best().at);
}

TEST_CASE("depth beyond capacity is refused and counted, not dropped") {
  test_book b;
  for (std::int64_t i = 0; i < 8; ++i) {
    REQUIRE(b.apply(update(true, at(10 + i, 0), 100)));
  }
  CHECK(b.bids().size() == 8);
  CHECK(b.bids().refused() == 0);

  const auto ninth = b.apply(update(true, at(5, 0), 100));
  CHECK_FALSE(ninth);
  CHECK(ninth.error_code() == dfr::error::capacity_exceeded);
  // A book that quietly stopped accepting depth would keep answering with an answer that used to be right.
  CHECK(b.bids().refused() == 1);
  CHECK(b.bids().size() == 8);
}

TEST_CASE("a trade does not move an aggregated book, and is counted") {
  test_book b;
  REQUIRE(b.apply(update(false, at(20, 9000), 500)));

  deep::trade_report trade;
  trade.symbol = "TEST";
  trade.size = 100;
  trade.at = at(20, 9000);
  b.observe(trade);

  // The size reduction arrives as its own price level update. A book that also decremented here would
  // double-count every fill.
  CHECK(b.asks().best().size == 500);
  CHECK(b.trades() == 1);
  CHECK(b.traded_shares() == 100);

  trade.broken = true;
  b.observe(trade);
  CHECK(b.trades() == 1);
  CHECK(b.broken_trades() == 1);
}

TEST_CASE("two books are equal exactly when both sides are") {
  test_book a;
  test_book c;
  REQUIRE(a.apply(update(true, at(20, 0), 100)));
  REQUIRE(a.apply(update(false, at(21, 0), 200)));
  CHECK_FALSE(a == c);

  REQUIRE(c.apply(update(false, at(21, 0), 200)));
  REQUIRE(c.apply(update(true, at(20, 0), 100)));
  // Order of arrival must not matter, only the resulting book. This is the operator the recovery oracle turns
  // on, so it has to compare state and not history.
  CHECK(a == c);

  REQUIRE(c.apply(update(true, at(20, 0), 101)));
  CHECK_FALSE(a == c);
}

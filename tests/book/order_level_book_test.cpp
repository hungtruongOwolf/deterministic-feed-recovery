#include <dfr/book/order_level_book.hpp>

#include <catch2/catch_test_macros.hpp>

namespace book = dfr::book;
namespace itch = dfr::wire::itch;

namespace {

constexpr itch::stock kStock{'N', 'V', 'D', 'A', ' ', ' ', ' ', ' '};

constexpr itch::add_order add(std::uint64_t reference, std::uint32_t shares = 100,
                              std::uint32_t price = 900'000, bool buy = true) {
  return itch::add_order{.order_reference = reference,
                         .symbol = kStock,
                         .shares = shares,
                         .price = price,
                         .buy = buy};
}

}  // namespace

TEST_CASE("an ITCH order lifecycle preserves identity and accounting", "[book][order-level]") {
  book::order_level_book<16> state;
  REQUIRE(state.apply(add(10, 100, 900'000)).has_value());
  REQUIRE(state.apply(add(20, 80, 901'000, false)).has_value());
  CHECK(state.size() == 2);
  CHECK(state.shares_open() == 180);

  REQUIRE(state.apply(itch::order_executed{.order_reference = 10,
                                            .match_number = 1001,
                                            .shares = 40})
              .has_value());
  REQUIRE(state.apply(itch::order_cancel{.order_reference = 10, .shares = 10})
              .has_value());
  REQUIRE(state.apply(itch::order_replace{.original_reference = 20,
                                           .new_reference = 21,
                                           .shares = 70,
                                           .price = 899'000})
              .has_value());

  CHECK(state.find(10)->shares == 50);
  CHECK(state.find(20) == nullptr);
  REQUIRE(state.find(21) != nullptr);
  CHECK_FALSE(state.find(21)->buy);
  CHECK(state.find(21)->stock == kStock);
  CHECK(state.find(21)->price == 899'000);
  CHECK(state.stats().executed_shares == 40);

  REQUIRE(state.apply(itch::order_delete{.order_reference = 21}).has_value());
  CHECK(state.size() == 1);
  CHECK(state.shares_open() == 50);
}

TEST_CASE("a full execution and cancel retire the order", "[book][order-level]") {
  book::order_level_book<8> state;
  REQUIRE(state.apply(add(1, 25)).has_value());
  REQUIRE(state.apply(itch::order_executed{.order_reference = 1, .shares = 25}).has_value());
  CHECK(state.find(1) == nullptr);

  REQUIRE(state.apply(add(2, 30)).has_value());
  REQUIRE(state.apply(itch::order_cancel{.order_reference = 2, .shares = 30}).has_value());
  CHECK(state.empty());
}

TEST_CASE("invalid order transitions are refused without changing state", "[book][order-level]") {
  book::order_level_book<8> state;
  REQUIRE(state.apply(add(1, 50)).has_value());
  const auto before = state;

  CHECK_FALSE(state.apply(add(1, 50)).has_value());
  CHECK_FALSE(state.apply(itch::order_executed{.order_reference = 1, .shares = 51})
                  .has_value());
  CHECK_FALSE(state.apply(itch::order_cancel{.order_reference = 99, .shares = 1})
                  .has_value());
  CHECK_FALSE(state.apply(itch::order_delete{.order_reference = 99}).has_value());
  CHECK_FALSE(state.apply(itch::order_replace{.original_reference = 1,
                                               .new_reference = 1,
                                               .shares = 10,
                                               .price = 1})
                  .has_value());
  CHECK(state == before);
}

TEST_CASE("tombstones preserve colliding lookup chains", "[book][order-level]") {
  book::order_level_book<8> state;
  // References separated by Capacity share a bucket because the hash multiplier is odd.
  REQUIRE(state.apply(add(1)).has_value());
  REQUIRE(state.apply(add(9)).has_value());
  REQUIRE(state.apply(add(17)).has_value());
  REQUIRE(state.apply(itch::order_delete{.order_reference = 9}).has_value());

  CHECK(state.find(1) != nullptr);
  CHECK(state.find(9) == nullptr);
  CHECK(state.find(17) != nullptr);
  REQUIRE(state.apply(add(25)).has_value());
  CHECK(state.find(25) != nullptr);
}

TEST_CASE("capacity is refused and a replace still fits a full book", "[book][order-level]") {
  book::order_level_book<8> state;
  for (std::uint64_t reference = 1; reference <= 8; ++reference) {
    REQUIRE(state.apply(add(reference)).has_value());
  }
  CHECK(state.apply(add(9)).error_code() == dfr::error::capacity_exceeded);

  REQUIRE(state.apply(itch::order_replace{.original_reference = 4,
                                           .new_reference = 40,
                                           .shares = 75,
                                           .price = 910'000})
              .has_value());
  CHECK(state.size() == 8);
  CHECK(state.find(4) == nullptr);
  REQUIRE(state.find(40) != nullptr);
  CHECK(state.find(40)->shares == 75);
}

TEST_CASE("books compare by live orders rather than hash layout", "[book][order-level]") {
  book::order_level_book<8> a;
  book::order_level_book<8> b;
  REQUIRE(a.apply(add(1)).has_value());
  REQUIRE(a.apply(add(9)).has_value());
  REQUIRE(b.apply(add(9)).has_value());
  REQUIRE(b.apply(add(1)).has_value());
  CHECK(a == b);

  REQUIRE(b.apply(itch::order_cancel{.order_reference = 1, .shares = 1}).has_value());
  CHECK_FALSE(a == b);
}

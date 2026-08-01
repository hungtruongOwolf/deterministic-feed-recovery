// The OUCH messages, and the three different things the Shares field means.

#include <dfr/wire/ouch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace ouch = dfr::wire::ouch;

namespace {

std::array<std::byte, 128> buffer{};

dfr::mutable_packet_view out() {
  buffer.fill(std::byte{0});
  return dfr::mutable_packet_view{buffer.data(), buffer.size()};
}

dfr::packet_view written(std::size_t size) {
  return dfr::packet_view{buffer.data(), size};
}

ouch::order_token token(std::string_view text) {
  ouch::order_token out_token;
  REQUIRE(ouch::order_token::from_text(text).get(out_token) == dfr::error::ok);
  return out_token;
}

ouch::price at_dollars(std::uint32_t dollars, std::uint32_t fraction = 0) {
  ouch::price p;
  REQUIRE(ouch::price::from_dollars_and_ten_thousandths(dollars, fraction).get(p) ==
          dfr::error::ok);
  return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// The Shares field means three different things
// ---------------------------------------------------------------------------

TEST_CASE("enter order shares are the size of the order", "[wire][ouch]") {
  ouch::enter_order order;
  order.token = token("ORD1");
  order.order_side = ouch::side::buy;
  order.shares = 500;
  order.stock = "AAPL";
  order.limit = at_dollars(150, 2'500);
  order.time_in_force = ouch::kSystemHours;
  order.firm = "FIRM";
  REQUIRE(order.validate().has_value());

  std::size_t size = 0;
  REQUIRE(ouch::encode_enter_order(out(), order).get(size) == dfr::error::ok);
  CHECK(size == 49);

  ouch::enter_order back;
  REQUIRE(ouch::decode_enter_order(written(size)).get(back) == dfr::error::ok);
  CHECK(back.token == order.token);
  CHECK(back.shares == 500);
  CHECK(back.stock == "AAPL");
  CHECK(back.limit == order.limit);
  CHECK(back.time_in_force == ouch::kSystemHours);
  CHECK(back.firm == "FIRM");
}

TEST_CASE("replace shares are cumulative across the whole chain",
          "[wire][ouch][regression]") {
  // §2.2's own example: enter 500, execute 100. A replace with 500 leaves 400 exposed; a replace with
  // 600 exposes 500. NASDAQ's stated reason is that it "inhibits the risk of double-liability
  // throughout the order/replace chain".
  //
  // An implementation reading this as "the new open quantity" doubles its exposure after every partial
  // fill, and nothing in the protocol complains. The field is named for its meaning so the mistake has
  // to be deliberate.
  ouch::replace_order replace;
  replace.existing_token = token("ORD1");
  replace.replacement_token = token("ORD2");
  replace.total_shares_liable = 500;  // keeps 400 exposed after 100 executed
  replace.limit = at_dollars(151);
  REQUIRE(replace.validate().has_value());

  std::size_t size = 0;
  REQUIRE(ouch::encode_replace_order(out(), replace).get(size) == dfr::error::ok);
  CHECK(size == 47);

  ouch::replace_order back;
  REQUIRE(ouch::decode_replace_order(written(size)).get(back) == dfr::error::ok);
  CHECK(back.existing_token == replace.existing_token);
  CHECK(back.replacement_token == replace.replacement_token);
  CHECK(back.total_shares_liable == 500);
}

TEST_CASE("cancel shares are the new intended size, not the amount to cancel",
          "[wire][ouch][regression]") {
  // §2.3: "This is the new intended order size... Entering a zero here will cancel any remaining open
  // shares." So a cancel with 100 on a 500-share order is a reduction to 100: reading it as "cancel
  // 100 shares" cancels four hundred too few.
  ouch::cancel_order reduce;
  reduce.token = token("ORD1");
  reduce.intended_order_size = 100;
  CHECK_FALSE(reduce.cancels_entirely());

  ouch::cancel_order fully;
  fully.token = token("ORD1");
  fully.intended_order_size = 0;
  CHECK(fully.cancels_entirely());

  std::size_t size = 0;
  REQUIRE(ouch::encode_cancel_order(out(), reduce).get(size) == dfr::error::ok);
  CHECK(size == 19);
  ouch::cancel_order back;
  REQUIRE(ouch::decode_cancel_order(written(size)).get(back) == dfr::error::ok);
  CHECK(back.intended_order_size == 100);
}

TEST_CASE("executed shares are incremental, not a running total",
          "[wire][ouch][regression]") {
  // §3.7: "Incremental number of shares executed". A client that assigned rather than accumulated
  // would read three fills of 100 on a 300-share order as an order still 200 short.
  const ouch::executed fill{.timestamp_ns = 1'000,
                            .token = token("ORD1"),
                            .shares_this_fill = 100,
                            .execution_price = at_dollars(150),
                            .liquidity_flag = ouch::reason_code{'R'},
                            .match_number = 77};
  std::size_t size = 0;
  REQUIRE(ouch::encode_executed(out(), fill).get(size) == dfr::error::ok);
  CHECK(size == 40);

  ouch::executed back;
  REQUIRE(ouch::decode_executed(written(size)).get(back) == dfr::error::ok);
  CHECK(back.shares_this_fill == 100);
  CHECK(back.match_number == 77);
  CHECK(ouch::removed_liquidity(back.liquidity_flag));
  CHECK_FALSE(ouch::added_liquidity(back.liquidity_flag));
}

TEST_CASE("canceled decrement shares are incremental too", "[wire][ouch]") {
  // §3.5 says so, and adds that a cancel "does not necessarily mean the entire order is dead".
  const ouch::canceled cancel{.timestamp_ns = 2'000,
                              .token = token("ORD1"),
                              .shares_decremented = 150,
                              .reason = ouch::reason_code{'U'}};
  std::size_t size = 0;
  REQUIRE(ouch::encode_canceled(out(), cancel).get(size) == dfr::error::ok);
  CHECK(size == 28);

  ouch::canceled back;
  REQUIRE(ouch::decode_canceled(written(size)).get(back) == dfr::error::ok);
  CHECK(back.shares_decremented == 150);
  CHECK(ouch::name_of_cancel_reason(back.reason) == "user_requested");
}

// ---------------------------------------------------------------------------
// The acknowledgements
// ---------------------------------------------------------------------------

TEST_CASE("an accepted message round-trips", "[wire][ouch]") {
  ouch::accepted ack;
  ack.token = token("ORD1");
  ack.shares_accepted = 500;
  ack.order.timestamp_ns = 9'000'000;
  ack.order.order_side = ouch::side::buy;
  ack.order.stock = "MSFT";
  ack.order.limit = at_dollars(300);
  ack.order.time_in_force = ouch::kMarketHours;
  ack.order.firm = "ABCD";
  ack.order.reference_number = 123'456'789;
  ack.order.account_capacity = ouch::capacity::principal;
  ack.order.state = ouch::order_state::live;

  std::size_t size = 0;
  REQUIRE(ouch::encode_accepted(out(), ack).get(size) == dfr::error::ok);
  CHECK(size == 66);

  ouch::accepted back;
  REQUIRE(ouch::decode_accepted(written(size)).get(back) == dfr::error::ok);
  CHECK(back.token == ack.token);
  CHECK(back.shares_accepted == 500);
  CHECK(back.order.stock == "MSFT");
  CHECK(back.order.reference_number == 123'456'789);
  CHECK(back.order.account_capacity == ouch::capacity::principal);
  CHECK(back.order.state == ouch::order_state::live);
  CHECK(back.order.time_in_force == ouch::kMarketHours);
}

TEST_CASE("a replaced message carries both tokens and the shares left exposed",
          "[wire][ouch][regression]") {
  // §3.4's in-flight example: enter 500, accept 500, replace 500, execute 100 on the *original*, and
  // the Replaced message reports 400. The client sent the replace before it knew about the execution,
  // and the exchange applied both: the order-entry equivalent of the Glimpse race.
  ouch::replaced ack;
  ack.replacement_token = token("ORD2");
  ack.previous_token = token("ORD1");
  ack.shares_outstanding = 400;  // not the 500 the replace asked for
  ack.order.timestamp_ns = 9'500'000;
  ack.order.stock = "MSFT";
  ack.order.limit = at_dollars(301);
  ack.order.reference_number = 987;
  ack.order.state = ouch::order_state::live;

  std::size_t size = 0;
  REQUIRE(ouch::encode_replaced(out(), ack).get(size) == dfr::error::ok);
  CHECK(size == 80);

  ouch::replaced back;
  REQUIRE(ouch::decode_replaced(written(size)).get(back) == dfr::error::ok);
  CHECK(back.replacement_token == token("ORD2"));
  CHECK(back.previous_token == token("ORD1"));
  CHECK(back.shares_outstanding == 400);
  CHECK(back.order.reference_number == 987);
}

// ---------------------------------------------------------------------------
// Validation and rejection
// ---------------------------------------------------------------------------

TEST_CASE("the documented share and time-in-force limits are enforced",
          "[wire][ouch]") {
  ouch::enter_order order;
  order.token = token("ORD1");
  order.stock = "AAPL";
  order.limit = at_dollars(10);

  order.shares = 0;
  CHECK(order.validate().error_code() == dfr::error::invalid_argument);
  order.shares = 1'000'000;  // the bound is exclusive
  CHECK(order.validate().error_code() == dfr::error::invalid_argument);
  order.shares = 999'999;
  CHECK(order.validate().has_value());

  order.time_in_force = 100'000;  // §1.2: larger than 99,999 is invalid
  CHECK(order.validate().error_code() == dfr::error::invalid_argument);
  order.time_in_force = ouch::kSystemHours;
  CHECK(order.validate().has_value());
}

TEST_CASE("immediate or cancel is a zero lifetime, not a flag",
          "[wire][ouch]") {
  // Worth pinning because a caller that reads Time in Force as "how long it lives" and leaves it at
  // zero has silently entered an IOC order that will never rest on the book.
  CHECK(ouch::is_immediate_or_cancel(0));
  CHECK_FALSE(ouch::is_immediate_or_cancel(ouch::kSystemHours));
  CHECK(ouch::is_valid_time_in_force(ouch::kExtendedTradingClose));
  CHECK_FALSE(ouch::is_valid_time_in_force(ouch::kMaxTimeInForce + 1));
}

TEST_CASE("a minimum quantity above the order size is refused",
          "[wire][ouch]") {
  // It could never be satisfied, so the order would rest unfillable. Reported here rather than left to
  // be discovered as a mystery non-execution.
  ouch::enter_order order;
  order.token = token("ORD1");
  order.stock = "AAPL";
  order.limit = at_dollars(10);
  order.shares = 100;
  order.minimum_quantity = 200;
  CHECK(order.validate().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a replace onto its own token is malformed", "[wire][ouch]") {
  // §2.2: replacement tokens may not repeat tokens already used. A replace onto itself is not a no-op.
  ouch::replace_order replace;
  replace.existing_token = token("ORD1");
  replace.replacement_token = token("ORD1");
  replace.total_shares_liable = 100;
  CHECK(replace.validate().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("the wrong type byte is reported rather than reinterpreted",
          "[wire][ouch]") {
  ouch::cancel_order cancel;
  cancel.token = token("ORD1");
  std::size_t size = 0;
  REQUIRE(ouch::encode_cancel_order(out(), cancel).get(size) == dfr::error::ok);

  // Same nineteen bytes, read as the message it is not.
  CHECK(ouch::decode_enter_order(written(size)).error_code() ==
        dfr::error::message_length_mismatch);
  buffer[0] = std::byte{'O'};
  CHECK(ouch::decode_cancel_order(written(size)).error_code() ==
        dfr::error::unknown_message_type);
}

TEST_CASE("an unknown reason code is named, not rejected",
          "[wire][ouch][regression]") {
  // The specification says clients "should anticipate additions to this list and thus support all
  // capital letters of the English alphabet", and NASDAQ has added to both reason lists repeatedly. A
  // client validating against a fixed set would start rejecting messages the day the exchange shipped
  // a new code, and would do it exactly when something unusual was happening to its orders.
  const ouch::reason_code invented{'~'};
  CHECK(ouch::name_of_cancel_reason(invented) == "unknown");
  CHECK(ouch::name_of_reject_reason(invented) == "unknown");
  CHECK(ouch::name_of_broken_reason(invented) == "unknown");

  const ouch::canceled cancel{.timestamp_ns = 1, .token = token("ORD1"),
                              .shares_decremented = 5, .reason = invented};
  std::size_t size = 0;
  REQUIRE(ouch::encode_canceled(out(), cancel).get(size) == dfr::error::ok);
  ouch::canceled back;
  REQUIRE(ouch::decode_canceled(written(size)).get(back) == dfr::error::ok);
  CHECK(back.reason == invented);
}

TEST_CASE("capacity values outside the documented three become Other",
          "[wire][ouch]") {
  // §2.1: "Values other than 'A', 'P', or 'R' will be converted to 'O' = Other". The conversion is the
  // exchange's, so refusing the message would reject something the exchange considers well formed.
  CHECK(ouch::capacity_from_wire('A') == ouch::capacity::agency);
  CHECK(ouch::capacity_from_wire('P') == ouch::capacity::principal);
  CHECK(ouch::capacity_from_wire('R') == ouch::capacity::riskless);
  CHECK(ouch::capacity_from_wire('Z') == ouch::capacity::other);
}

TEST_CASE("modify may only exchange the short variants", "[wire][ouch]") {
  // §3.4 lists the permitted transitions: S→T, S→E, E→T, E→S, T→E, T→S. A buy cannot become a sell.
  CHECK(ouch::is_permitted_modify_transition(ouch::side::sell, ouch::side::sell_short));
  CHECK(ouch::is_permitted_modify_transition(ouch::side::sell_short_exempt, ouch::side::sell));
  CHECK_FALSE(ouch::is_permitted_modify_transition(ouch::side::buy, ouch::side::sell));
  CHECK_FALSE(ouch::is_permitted_modify_transition(ouch::side::sell, ouch::side::buy));
  CHECK_FALSE(ouch::is_permitted_modify_transition(ouch::side::sell, ouch::side::sell));
}

TEST_CASE("cancel pending and cancel reject share a layout and differ in kind",
          "[wire][ouch]") {
  // Pending: it will be canceled automatically after the cross. Reject: nothing is scheduled and the
  // client may ask again. Same two fields, opposite instructions.
  for (const bool pending : {true, false}) {
    const ouch::cancel_notice notice{.timestamp_ns = 5, .token = token("ORD1"),
                                     .pending = pending};
    std::size_t size = 0;
    REQUIRE(ouch::encode_cancel_notice(out(), notice).get(size) == dfr::error::ok);
    CHECK(size == 23);
    ouch::cancel_notice back;
    REQUIRE(ouch::decode_cancel_notice(written(size)).get(back) == dfr::error::ok);
    CHECK(back.pending == pending);
    CHECK(back.token == token("ORD1"));
  }
}

TEST_CASE("every remaining outbound message round-trips", "[wire][ouch]") {
  std::size_t size = 0;

  const ouch::system_event event{.timestamp_ns = 1, .code = ouch::event_code::start_of_day};
  REQUIRE(ouch::encode_system_event(out(), event).get(size) == dfr::error::ok);
  ouch::system_event event_back;
  REQUIRE(ouch::decode_system_event(written(size)).get(event_back) == dfr::error::ok);
  CHECK(event_back.code == ouch::event_code::start_of_day);

  const ouch::rejected reject{.timestamp_ns = 2, .token = token("ORD1"),
                              .reason = ouch::reason_code{'S'}};
  REQUIRE(ouch::encode_rejected(out(), reject).get(size) == dfr::error::ok);
  ouch::rejected reject_back;
  REQUIRE(ouch::decode_rejected(written(size)).get(reject_back) == dfr::error::ok);
  CHECK(ouch::name_of_reject_reason(reject_back.reason) == "invalid_stock");

  const ouch::broken_trade broken{.timestamp_ns = 3, .token = token("ORD1"),
                                  .match_number = 42,
                                  .reason = ouch::reason_code{'E'}};
  REQUIRE(ouch::encode_broken_trade(out(), broken).get(size) == dfr::error::ok);
  ouch::broken_trade broken_back;
  REQUIRE(ouch::decode_broken_trade(written(size)).get(broken_back) == dfr::error::ok);
  CHECK(broken_back.match_number == 42);
  CHECK(ouch::name_of_broken_reason(broken_back.reason) == "erroneous");

  const ouch::modified change{.timestamp_ns = 4, .token = token("ORD1"),
                              .order_side = ouch::side::sell_short,
                              .shares_outstanding = 250};
  REQUIRE(ouch::encode_modified(out(), change).get(size) == dfr::error::ok);
  ouch::modified change_back;
  REQUIRE(ouch::decode_modified(written(size)).get(change_back) == dfr::error::ok);
  CHECK(change_back.shares_outstanding == 250);
  CHECK(change_back.order_side == ouch::side::sell_short);

  const ouch::priority_update priority{.timestamp_ns = 5, .token = token("ORD1"),
                                       .limit = at_dollars(99), .display = 'Y',
                                       .reference_number = 555};
  REQUIRE(ouch::encode_priority_update(out(), priority).get(size) == dfr::error::ok);
  ouch::priority_update priority_back;
  REQUIRE(ouch::decode_priority_update(written(size)).get(priority_back) ==
          dfr::error::ok);
  CHECK(priority_back.reference_number == 555);
  CHECK(priority_back.limit == at_dollars(99));
}

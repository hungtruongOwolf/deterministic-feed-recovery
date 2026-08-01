// The closed-set OUCH fields: side, order_state, event_code, and the names of their values.
//
// Found the same way tests/venue/order_session_state_test.cpp was: measuring coverage rather than reading.
// name_of(side), name_of(order_state) and name_of(event_code) had never run, unlike their siblings
// name_of_cancel_reason/_reject_reason/_broken_reason a few lines further down the same file, which
// ouch_message_test.cpp and order_entry_test.cpp both call directly. These three had no caller at all, in
// tests or in tools.

#include <dfr/wire/ouch/enums.hpp>

#include <catch2/catch_test_macros.hpp>

namespace ouch = dfr::wire::ouch;

TEST_CASE("every side has a name, and is_known agrees with the enum", "[wire][ouch]") {
  CHECK(ouch::name_of(ouch::side::buy) == "buy");
  CHECK(ouch::name_of(ouch::side::sell) == "sell");
  CHECK(ouch::name_of(ouch::side::sell_short) == "sell_short");
  CHECK(ouch::name_of(ouch::side::sell_short_exempt) == "sell_short_exempt");

  CHECK(ouch::is_known(ouch::side::buy));
  CHECK(ouch::is_known(ouch::side::sell));
  CHECK(ouch::is_known(ouch::side::sell_short));
  CHECK(ouch::is_known(ouch::side::sell_short_exempt));
}

TEST_CASE("only the three short variants may replace one another", "[wire][ouch]") {
  // Buy cannot become a sell and a sell cannot become a buy: only sell/sell_short/sell_short_exempt may be
  // exchanged for one another, per §3.4.
  CHECK_FALSE(ouch::is_permitted_modify_transition(ouch::side::buy, ouch::side::sell));
  CHECK_FALSE(ouch::is_permitted_modify_transition(ouch::side::sell, ouch::side::buy));
  CHECK_FALSE(ouch::is_permitted_modify_transition(ouch::side::sell, ouch::side::sell));

  CHECK(ouch::is_permitted_modify_transition(ouch::side::sell, ouch::side::sell_short));
  CHECK(ouch::is_permitted_modify_transition(ouch::side::sell_short, ouch::side::sell_short_exempt));
  CHECK(ouch::is_permitted_modify_transition(ouch::side::sell_short_exempt, ouch::side::sell));
}

TEST_CASE("every order state has a name, and is_known agrees with the enum", "[wire][ouch]") {
  CHECK(ouch::name_of(ouch::order_state::live) == "live");
  CHECK(ouch::name_of(ouch::order_state::dead) == "dead");
  CHECK(ouch::is_known(ouch::order_state::live));
  CHECK(ouch::is_known(ouch::order_state::dead));
}

TEST_CASE("every system event has a name, and is_known agrees with the enum", "[wire][ouch]") {
  CHECK(ouch::name_of(ouch::event_code::start_of_day) == "start_of_day");
  CHECK(ouch::name_of(ouch::event_code::end_of_day) == "end_of_day");
  CHECK(ouch::is_known(ouch::event_code::start_of_day));
  CHECK(ouch::is_known(ouch::event_code::end_of_day));
}

// Replacing and executing: the four outcomes of a replace, and the accounting identity.
//
// The token asymmetry between outcomes 2 and 3 of OUCH §2.2 is the part worth reading twice: an
// invalid-details replace leaves the replacement token reusable and a rejection burns it, and the
// in-flight execution is the order-entry equivalent of the Glimpse race.

#include <dfr/venue/order_entry.hpp>

#include "venue/support/order_entry_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace ouch = dfr::wire::ouch;
namespace venue = dfr::venue;

using dfr_test::order_entry::at_dollars;
using dfr_test::order_entry::at_ns;
using dfr_test::order_entry::buy;
using dfr_test::order_entry::fresh_host;
using dfr_test::order_entry::sink;
using dfr_test::order_entry::token;

// ---------------------------------------------------------------------------
// Replacing: the four outcomes, and the token asymmetry
// ---------------------------------------------------------------------------

TEST_CASE("a replace carries the executed count forward",
          "[venue][ouch][regression]") {
  // §2.2's own example. Enter 500, execute 100, replace with 500, and 400 is left exposed, because the
  // replace's Shares is the total liable for the *whole chain, inclusive of prior executions*. NASDAQ's
  // stated reason is that it "inhibits the risk of double-liability".
  //
  // A host reading it as "the new open quantity" would expose 500 on top of the 100 already done.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);
  REQUIRE(host.execute(token("ORD1"), 100, at_dollars(150), at_ns(2), out)
              .get(outcome) == dfr::error::ok);

  ouch::replace_order replace;
  replace.existing_token = token("ORD1");
  replace.replacement_token = token("ORD2");
  replace.total_shares_liable = 500;
  replace.limit = at_dollars(151);
  replace.time_in_force = ouch::kSystemHours;
  REQUIRE(host.replace(replace, at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::replaced);

  ouch::replaced ack;
  REQUIRE(ouch::decode_replaced(out.at(2)).get(ack) == dfr::error::ok);
  CHECK(ack.replacement_token == token("ORD2"));
  CHECK(ack.previous_token == token("ORD1"));
  CHECK(ack.shares_outstanding == 400);  // 500 liable minus 100 executed
  CHECK(ack.order.reference_number == 2);  // a replace gets a new reference number
  CHECK(host.accounts());

  // And asking for 600 exposes 500, which is the other half of the example.
  ouch::replace_order more;
  more.existing_token = token("ORD2");
  more.replacement_token = token("ORD3");
  more.total_shares_liable = 600;
  more.limit = at_dollars(151);
  more.time_in_force = ouch::kSystemHours;
  REQUIRE(host.replace(more, at_ns(4), out).get(outcome) == dfr::error::ok);
  REQUIRE(ouch::decode_replaced(out.at(3)).get(ack) == dfr::error::ok);
  CHECK(ack.shares_outstanding == 500);
  CHECK(host.accounts());
}

TEST_CASE("a replace crossing an execution reports what is actually exposed",
          "[venue][ouch][regression]") {
  // §3.4's in-flight scenario, and the order-entry equivalent of the Glimpse race: the client sends a
  // replace for 500, an execution for 100 lands on the *original* before it is processed, and the
  // Replaced message reports 400. The client asked for 500 and got 400, correctly.
  //
  // A client that assumed its replace would report the number it asked for would be carrying a hundred
  // shares it did not know about.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);

  // The replace is formed here, before anything has executed.
  ouch::replace_order replace;
  replace.existing_token = token("ORD1");
  replace.replacement_token = token("ORD2");
  replace.total_shares_liable = 500;
  replace.limit = at_dollars(150);
  replace.time_in_force = ouch::kSystemHours;

  // …and while it is in flight, the original fills 100.
  REQUIRE(host.execute(token("ORD1"), 100, at_dollars(150), at_ns(2), out)
              .get(outcome) == dfr::error::ok);

  REQUIRE(host.replace(replace, at_ns(3), out).get(outcome) == dfr::error::ok);
  ouch::replaced ack;
  REQUIRE(ouch::decode_replaced(out.at(2)).get(ack) == dfr::error::ok);
  CHECK(ack.shares_outstanding == 400);
  CHECK(host.accounts());
}

TEST_CASE("invalid replace details cancel the existing order and spare the token",
          "[venue][ouch][regression]") {
  // §2.2 outcome 2: "a Canceled Order Message will take the existing order out of the book. The
  // replacement Order Token will not be consumed, and may be reused in this case."
  //
  // The asymmetry with outcome 3 below is the part worth reading twice. Getting them the wrong way round
  // either refuses a client's legitimate retry or accepts a duplicate as a new order.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);

  ouch::replace_order bad;
  bad.existing_token = token("ORD1");
  bad.replacement_token = token("ORD2");
  bad.total_shares_liable = 1'000'000;  // the bound is exclusive
  bad.limit = at_dollars(150);
  bad.time_in_force = ouch::kSystemHours;
  REQUIRE(host.replace(bad, at_ns(2), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::replace_canceled_existing);

  ouch::canceled cancel;
  REQUIRE(ouch::decode_canceled(out.at(1)).get(cancel) == dfr::error::ok);
  CHECK(cancel.shares_decremented == 500);
  CHECK_FALSE(host.find(token("ORD1"))->live);

  // The replacement token was spared, so the client may reuse it.
  CHECK_FALSE(host.tokens().is_used(token("ORD2")));
  REQUIRE(host.enter(buy("ORD2", 200), at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::accepted);
  CHECK(host.accounts());
}

TEST_CASE("a replace of an uncancelable order is rejected and burns the token",
          "[venue][ouch][regression]") {
  // §2.2 outcome 3: "there will be a Reject Message... The Reject Message consumes the replacement Order
  // Token, so the replacement Order Token may not be reused." The existing order is left fully intact.
  auto host = fresh_host(/*late_cross=*/true);
  sink out;
  venue::order_outcome outcome{};
  auto cross = buy("ORD1", 500);
  cross.cross_type = 'O';  // an opening cross order
  REQUIRE(host.enter(cross, at_ns(1), out).get(outcome) == dfr::error::ok);

  ouch::replace_order replace;
  replace.existing_token = token("ORD1");
  replace.replacement_token = token("ORD2");
  replace.total_shares_liable = 400;
  replace.limit = at_dollars(150);
  replace.time_in_force = ouch::kSystemHours;
  REQUIRE(host.replace(replace, at_ns(2), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::rejected);

  ouch::rejected reject;
  REQUIRE(ouch::decode_rejected(out.at(1)).get(reject) == dfr::error::ok);
  CHECK(reject.token == token("ORD2"));

  // The existing order is untouched…
  const auto* order = host.find(token("ORD1"));
  REQUIRE(order != nullptr);
  CHECK(order->live);
  CHECK(order->shares_open() == 500);

  // …and the replacement token is spent, so a retry with it is ignored.
  CHECK(host.tokens().is_used(token("ORD2")));
  REQUIRE(host.enter(buy("ORD2", 100), at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
}

TEST_CASE("a replace against a dead order is silently ignored", "[venue][ouch]") {
  // §2.2 outcome 1. Silence, and the replacement token is not consumed, because nothing happened.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 100), at_ns(1), out).get(outcome) == dfr::error::ok);
  ouch::cancel_order kill;
  kill.token = token("ORD1");
  REQUIRE(host.cancel(kill, at_ns(2), out).get(outcome) == dfr::error::ok);
  const std::size_t before = out.count();

  ouch::replace_order replace;
  replace.existing_token = token("ORD1");
  replace.replacement_token = token("ORD2");
  replace.total_shares_liable = 100;
  replace.limit = at_dollars(150);
  replace.time_in_force = ouch::kSystemHours;
  REQUIRE(host.replace(replace, at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
  CHECK(out.count() == before);
  CHECK_FALSE(host.tokens().is_used(token("ORD2")));
}

TEST_CASE("a replace asking for less than already executed is invalid details",
          "[venue][ouch]") {
  // Derived rather than quoted: if the chain's total liable were below what has already executed, the
  // shares outstanding would be negative. That is exactly the "details are invalid" case, so it takes
  // the existing order out of the book and spares the token.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);
  REQUIRE(host.execute(token("ORD1"), 300, at_dollars(150), at_ns(2), out)
              .get(outcome) == dfr::error::ok);

  ouch::replace_order shrink;
  shrink.existing_token = token("ORD1");
  shrink.replacement_token = token("ORD2");
  shrink.total_shares_liable = 200;  // below the 300 already executed
  shrink.limit = at_dollars(150);
  shrink.time_in_force = ouch::kSystemHours;
  REQUIRE(host.replace(shrink, at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::replace_canceled_existing);
  CHECK_FALSE(host.tokens().is_used(token("ORD2")));
  CHECK(host.accounts());
}

// ---------------------------------------------------------------------------
// Executions and the identity
// ---------------------------------------------------------------------------

TEST_CASE("executions accumulate and the identity holds throughout",
          "[venue][ouch]") {
  // The order-side equivalent of the recovery accounting identity: every share ever made liable is
  // executed, canceled, or still open. It is what makes a wrong reading of the Shares field detectable
  // rather than merely expensive.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);

  for (int fill = 0; fill < 4; ++fill) {
    REQUIRE(host.execute(token("ORD1"), 100, at_dollars(150),
                         at_ns(10 + fill), out)
                .get(outcome) == dfr::error::ok);
    REQUIRE(host.accounts());
  }

  const auto* order = host.find(token("ORD1"));
  REQUIRE(order != nullptr);
  CHECK(order->shares_executed == 400);
  CHECK(order->shares_open() == 100);
  CHECK(order->live);
  CHECK(host.stats().shares_executed == 400);

  // Each Executed message is one fill, not a running total, and each gets its own match number.
  ouch::executed first;
  ouch::executed last;
  REQUIRE(ouch::decode_executed(out.at(1)).get(first) == dfr::error::ok);
  REQUIRE(ouch::decode_executed(out.at(4)).get(last) == dfr::error::ok);
  CHECK(first.shares_this_fill == 100);
  CHECK(last.shares_this_fill == 100);
  CHECK(first.match_number == 1);
  CHECK(last.match_number == 4);
}

TEST_CASE("filling the last share closes the order", "[venue][ouch]") {
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 100), at_ns(1), out).get(outcome) == dfr::error::ok);
  REQUIRE(host.execute(token("ORD1"), 100, at_dollars(150), at_ns(2), out)
              .get(outcome) == dfr::error::ok);

  const auto* order = host.find(token("ORD1"));
  REQUIRE(order != nullptr);
  CHECK_FALSE(order->live);
  CHECK(order->shares_open() == 0);

  // And a cancel afterwards is silence, because there is nothing left to cancel.
  ouch::cancel_order late;
  late.token = token("ORD1");
  REQUIRE(host.cancel(late, at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
}

TEST_CASE("an execution larger than what is exposed is refused",
          "[venue][ouch][regression]") {
  // It would break the identity. Refused rather than clamped: a venue that silently trimmed a fill would
  // be hiding a defect in whatever produced it.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 100), at_ns(1), out).get(outcome) == dfr::error::ok);

  const auto refused = host.execute(token("ORD1"), 200, at_dollars(150), at_ns(2), out);
  CHECK(refused.error_code() == dfr::error::invalid_argument);
  CHECK(host.accounts());
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

TEST_CASE("a modify may only exchange the short variants", "[venue][ouch]") {
  // §3.4 lists the permitted transitions and nothing else. The specification does not say what happens
  // to a transition outside them, so the host ignores it rather than inventing a rejection, and the
  // choice is recorded rather than left implicit.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  auto sell = buy("ORD1", 500);
  sell.order_side = ouch::side::sell;
  REQUIRE(host.enter(sell, at_ns(1), out).get(outcome) == dfr::error::ok);

  ouch::modify_order to_short;
  to_short.token = token("ORD1");
  to_short.order_side = ouch::side::sell_short;
  to_short.total_shares_liable = 500;
  REQUIRE(host.modify(to_short, at_ns(2), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::modified);

  ouch::modified change;
  REQUIRE(ouch::decode_modified(out.at(1)).get(change) == dfr::error::ok);
  CHECK(change.order_side == ouch::side::sell_short);
  CHECK(change.shares_outstanding == 500);

  ouch::modify_order to_buy;
  to_buy.token = token("ORD1");
  to_buy.order_side = ouch::side::buy;
  to_buy.total_shares_liable = 500;
  const std::size_t before = out.count();
  REQUIRE(host.modify(to_buy, at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
  CHECK(out.count() == before);
}

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

TEST_CASE("a long day of orders keeps every token distinct", "[venue][ouch]") {
  // The registry is a sorted flat array searched by bisection: no map, no hashing, no allocation, the
  // same decision docs/DESIGN.md §0 records for the recovery channel table. This exercises it enough to
  // catch an insertion that lost order.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  for (int i = 0; i < 300; ++i) {
    const std::string text = "ORD" + std::to_string(i);
    REQUIRE(host.enter(buy(text, 100), at_ns(i), out).get(outcome) == dfr::error::ok);
    REQUIRE(outcome == venue::order_outcome::accepted);
  }
  CHECK(host.tokens().size() == 300);
  CHECK(host.accounts());

  // Every one of them is now a duplicate.
  for (int i = 0; i < 300; ++i) {
    const std::string text = "ORD" + std::to_string(i);
    REQUIRE(host.enter(buy(text, 100), at_ns(1'000 + i), out).get(outcome) ==
            dfr::error::ok);
    REQUIRE(outcome == venue::order_outcome::ignored);
  }
  CHECK(host.stats().ignored_duplicate_token == 300);
}

TEST_CASE("every outcome has a distinct name", "[venue][ouch]") {
  CHECK(venue::name_of(venue::order_outcome::ignored) == "ignored");
  CHECK(venue::name_of(venue::order_outcome::accepted) == "accepted");
  CHECK(venue::name_of(venue::order_outcome::rejected) == "rejected");
  CHECK(venue::name_of(venue::order_outcome::replaced) == "replaced");
  CHECK(venue::name_of(venue::order_outcome::replace_canceled_existing) ==
        "replace_canceled_existing");
  CHECK(venue::name_of(venue::order_outcome::canceled) == "canceled");
  CHECK(venue::name_of(venue::order_outcome::modified) == "modified");
  CHECK(venue::name_of(venue::order_outcome::executed) == "executed");
}

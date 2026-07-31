// Entering and cancelling: the token rules, and what silence means.
//
// Every message the host emits is decoded before it is checked, so the encoder and the decoder verify
// each other — the arrangement venue::iextp_publisher uses, and the reason a host handing back structs
// would only ever agree with itself.
//
// Replacing and executing are in order_replace_test.cpp: those are about the chain's arithmetic, while
// these are about identity and acknowledgement.

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
// Entering
// ---------------------------------------------------------------------------

TEST_CASE("a valid order is accepted and echoed back", "[venue][ouch]") {
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1'000), out).get(outcome) ==
          dfr::error::ok);

  CHECK(outcome == venue::order_outcome::accepted);
  REQUIRE(out.count() == 1);
  CHECK(out.type_at(0) == 'A');

  ouch::accepted ack;
  REQUIRE(ouch::decode_accepted(out.at(0)).get(ack) == dfr::error::ok);
  CHECK(ack.token == token("ORD1"));
  CHECK(ack.shares_accepted == 500);
  CHECK(ack.order.stock == "AAPL");
  CHECK(ack.order.state == ouch::order_state::live);
  CHECK(ack.order.reference_number == 1);
  CHECK(ack.order.timestamp_ns == 1'000);
  CHECK(host.accounts());
}

TEST_CASE("a duplicate token is met with silence, not a rejection",
          "[venue][ouch][regression]") {
  // §2.1: "If you send an Enter Order Message with a previously used Order Token, the new order will be
  // ignored." Silence, because re-sending after a connection loss is the protocol's own recovery
  // mechanism — §2 permits repeating any inbound message — and answering with a rejection would make a
  // successful failover look like a failure.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);
  REQUIRE(outcome == venue::order_outcome::accepted);

  REQUIRE(host.enter(buy("ORD1", 900), at_ns(2), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
  CHECK(out.count() == 1);  // nothing more went out
  CHECK(host.stats().ignored_duplicate_token == 1);

  // And the first order is untouched: the duplicate did not resize it.
  const auto* order = host.find(token("ORD1"));
  REQUIRE(order != nullptr);
  CHECK(order->shares_liable == 500);
}

TEST_CASE("a rejection consumes the token", "[venue][ouch][regression]") {
  // §3.10: "The Order Token of a Rejected Message cannot be re-used." So a client that fixes its mistake
  // must pick a new token, and a host that let the old one through would accept two orders the client
  // considers one.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};

  auto bad = buy("ORD1", 0);  // shares must be greater than zero
  REQUIRE(host.enter(bad, at_ns(1), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::rejected);
  REQUIRE(out.count() == 1);
  CHECK(out.type_at(0) == 'J');

  ouch::rejected reject;
  REQUIRE(ouch::decode_rejected(out.at(0)).get(reject) == dfr::error::ok);
  CHECK(reject.token == token("ORD1"));
  CHECK(ouch::name_of_reject_reason(reject.reason) == "shares_exceed_safety_threshold");

  // Retrying the same token, now valid, is ignored rather than accepted.
  REQUIRE(host.enter(buy("ORD1", 100), at_ns(2), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
  CHECK(out.count() == 1);
}

TEST_CASE("the rejection reason is the documented one", "[venue][ouch]") {
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};

  auto no_stock = buy("ORD1", 100);
  no_stock.stock = "";
  REQUIRE(host.enter(no_stock, at_ns(1), out).get(outcome) == dfr::error::ok);
  ouch::rejected reject;
  REQUIRE(ouch::decode_rejected(out.at(0)).get(reject) == dfr::error::ok);
  CHECK(ouch::name_of_reject_reason(reject.reason) == "invalid_stock");

  auto bad_minimum = buy("ORD2", 100);
  bad_minimum.minimum_quantity = 200;
  REQUIRE(host.enter(bad_minimum, at_ns(2), out).get(outcome) == dfr::error::ok);
  REQUIRE(ouch::decode_rejected(out.at(1)).get(reject) == dfr::error::ok);
  CHECK(ouch::name_of_reject_reason(reject.reason) == "invalid_minimum_quantity");
}

TEST_CASE("an immediate-or-cancel order is accepted dead, not rejected",
          "[venue][ouch]") {
  // §3.3: an Accepted message whose Order State is "D" means the order was accepted and automatically
  // canceled, and no further messages follow. That is not a rejection, and a client told otherwise would
  // retry an order the exchange had taken.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  auto ioc = buy("IOC1", 300);
  ioc.time_in_force = ouch::kImmediateOrCancel;

  REQUIRE(host.enter(ioc, at_ns(1), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::accepted);
  ouch::accepted ack;
  REQUIRE(ouch::decode_accepted(out.at(0)).get(ack) == dfr::error::ok);
  CHECK(ack.order.state == ouch::order_state::dead);
  CHECK(ack.shares_accepted == 300);

  const auto* order = host.find(token("IOC1"));
  REQUIRE(order != nullptr);
  CHECK(order->shares_open() == 0);
  CHECK(host.accounts());
}

TEST_CASE("a blank firm becomes the account default", "[venue][ouch]") {
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  auto order = buy("ORD1", 100);
  order.firm = "";
  REQUIRE(host.enter(order, at_ns(1), out).get(outcome) == dfr::error::ok);

  ouch::accepted ack;
  REQUIRE(ouch::decode_accepted(out.at(0)).get(ack) == dfr::error::ok);
  CHECK(ack.order.firm == "DFLT");
}

// ---------------------------------------------------------------------------
// Cancelling
// ---------------------------------------------------------------------------

TEST_CASE("a cancel reduces to the intended size", "[venue][ouch][regression]") {
  // §2.3: the Shares field is "the new intended order size", not the amount to cancel. So 100 on a
  // 500-share order removes 400 — an implementation reading it the other way would remove 100 and leave
  // the client four hundred shares more exposed than it asked for.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);

  ouch::cancel_order reduce;
  reduce.token = token("ORD1");
  reduce.intended_order_size = 100;
  REQUIRE(host.cancel(reduce, at_ns(2), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::canceled);

  ouch::canceled cancel;
  REQUIRE(ouch::decode_canceled(out.at(1)).get(cancel) == dfr::error::ok);
  CHECK(cancel.shares_decremented == 400);
  CHECK(ouch::name_of_cancel_reason(cancel.reason) == "user_requested");

  const auto* order = host.find(token("ORD1"));
  REQUIRE(order != nullptr);
  CHECK(order->shares_open() == 100);
  CHECK(order->live);
  CHECK(host.accounts());
}

TEST_CASE("a cancel with zero shares cancels everything open", "[venue][ouch]") {
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);

  ouch::cancel_order fully;
  fully.token = token("ORD1");
  fully.intended_order_size = 0;
  REQUIRE(host.cancel(fully, at_ns(2), out).get(outcome) == dfr::error::ok);

  ouch::canceled cancel;
  REQUIRE(ouch::decode_canceled(out.at(1)).get(cancel) == dfr::error::ok);
  CHECK(cancel.shares_decremented == 500);
  const auto* order = host.find(token("ORD1"));
  REQUIRE(order != nullptr);
  CHECK_FALSE(order->live);
  CHECK(host.accounts());
}

TEST_CASE("a cancel cannot reduce below what has executed",
          "[venue][ouch][regression]") {
  // Those shares are gone rather than open, so a cancel to zero on a partly filled order removes only
  // what is left. A host that took the intended size literally would report a decrement larger than the
  // order and break the accounting identity.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 500), at_ns(1), out).get(outcome) == dfr::error::ok);
  REQUIRE(host.execute(token("ORD1"), 200, at_dollars(150), at_ns(2), out)
              .get(outcome) == dfr::error::ok);

  ouch::cancel_order fully;
  fully.token = token("ORD1");
  fully.intended_order_size = 0;
  REQUIRE(host.cancel(fully, at_ns(3), out).get(outcome) == dfr::error::ok);

  ouch::canceled cancel;
  REQUIRE(ouch::decode_canceled(out.at(2)).get(cancel) == dfr::error::ok);
  CHECK(cancel.shares_decremented == 300);  // not 500
  const auto* order = host.find(token("ORD1"));
  REQUIRE(order != nullptr);
  CHECK(order->shares_executed == 200);
  CHECK(order->shares_canceled == 300);
  CHECK(order->shares_open() == 0);
  CHECK(host.accounts());
}

TEST_CASE("a superfluous cancel is silently ignored",
          "[venue][ouch][regression]") {
  // §2.3: "Superfluous Cancel Order Messages are silently ignored... There is no 'too late to cancel'
  // message since by the time you received it, you would already have gotten the execution."
  //
  // So silence after a cancel is the exchange saying the order was already gone. A client that retried
  // on silence would generate cancels forever, and a host that answered them would be modelling an
  // exchange that does not exist.
  auto host = fresh_host();
  sink out;
  venue::order_outcome outcome{};
  REQUIRE(host.enter(buy("ORD1", 100), at_ns(1), out).get(outcome) == dfr::error::ok);

  ouch::cancel_order fully;
  fully.token = token("ORD1");
  fully.intended_order_size = 0;
  REQUIRE(host.cancel(fully, at_ns(2), out).get(outcome) == dfr::error::ok);
  const std::size_t after_first = out.count();

  // The same cancel again, and a cancel for a token that never existed.
  REQUIRE(host.cancel(fully, at_ns(3), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
  ouch::cancel_order unknown;
  unknown.token = token("NOPE");
  REQUIRE(host.cancel(unknown, at_ns(4), out).get(outcome) == dfr::error::ok);
  CHECK(outcome == venue::order_outcome::ignored);
  CHECK(out.count() == after_first);
}


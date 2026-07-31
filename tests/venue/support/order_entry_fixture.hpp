// Shared setup for the OUCH host tests.
//
// Two files drive one host — entering and cancelling, then replacing and executing — so the sink and the
// message builders live here rather than being copied, per docs/STYLE.md §1.10.

#ifndef DFR_TESTS_VENUE_SUPPORT_ORDER_ENTRY_FIXTURE_HPP
#define DFR_TESTS_VENUE_SUPPORT_ORDER_ENTRY_FIXTURE_HPP

#include <dfr/venue/order_entry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dfr_test::order_entry {

namespace ouch = dfr::wire::ouch;
namespace venue = dfr::venue;

using test_host = venue::order_entry<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

namespace {


inline test_time at_ns(std::int64_t nanos) {
  return test_time{} + std::chrono::nanoseconds{nanos};
}

inline ouch::order_token token(std::string_view text) {
  ouch::order_token out;
  REQUIRE(ouch::order_token::from_text(text).get(out) == dfr::error::ok);
  return out;
}

inline ouch::price at_dollars(std::uint32_t dollars) {
  ouch::price out;
  REQUIRE(ouch::price::from_dollars_and_ten_thousandths(dollars, 0).get(out) ==
          dfr::error::ok);
  return out;
}

// Collects what the host emitted, copied because the host reuses one scratch buffer.
struct sink {
  std::vector<std::string> messages;

  void operator()(dfr::packet_view message) {
    messages.emplace_back(reinterpret_cast<const char*>(message.data()),
                          message.size());
  }

  [[nodiscard]] dfr::packet_view at(std::size_t i) const {
    return dfr::packet_view{messages[i].data(), messages[i].size()};
  }
  [[nodiscard]] char type_at(std::size_t i) const { return messages[i][0]; }
  [[nodiscard]] std::size_t count() const { return messages.size(); }
};

inline ouch::enter_order buy(std::string_view text, std::uint32_t shares,
                      std::uint32_t dollars = 150) {
  ouch::enter_order order;
  order.token = token(text);
  order.order_side = ouch::side::buy;
  order.shares = shares;
  order.stock = "AAPL";
  order.limit = at_dollars(dollars);
  order.time_in_force = ouch::kSystemHours;
  order.firm = "FIRM";
  return order;
}

inline test_host fresh_host(bool late_cross = false) {
  venue::order_entry_options options;
  options.late_cross_period = late_cross;
  return test_host{options};
}

}  // namespace

}  // namespace dfr_test::order_entry

#endif  // DFR_TESTS_VENUE_SUPPORT_ORDER_ENTRY_FIXTURE_HPP

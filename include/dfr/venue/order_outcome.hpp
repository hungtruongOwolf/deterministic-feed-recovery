// What an OUCH host did with an inbound message, and how it is configured.
//
// Split from order_entry.hpp on the seam docs/STYLE.md §1.10 asks for, and the same one
// recovery::observation and recovery::request_decision use: a caller that logs an outcome or asserts on
// one in a test needs this vocabulary and never needs the state machine.

#ifndef DFR_VENUE_ORDER_OUTCOME_HPP
#define DFR_VENUE_ORDER_OUTCOME_HPP

#include <dfr/core/assert.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace venue {

// What the host did with an inbound message. Returned alongside whatever was emitted, because several
// outcomes emit *nothing* and a caller cannot tell those apart from the wire.
enum class order_outcome : std::uint8_t {
  // §2.1/§2.2/§2.3: a duplicate token, a replace against a dead order, or a superfluous cancel. The
  // protocol's answer is silence, which is why this is a return value rather than a message.
  ignored,
  accepted,
  rejected,
  replaced,
  // A replace whose details were invalid: the existing order is canceled and the replacement token is
  // *not* consumed.
  replace_canceled_existing,
  canceled,
  modified,
  executed,

  count_
};

[[nodiscard]] constexpr std::string_view name_of(order_outcome value) noexcept {
  switch (value) {
    case order_outcome::ignored:                   return "ignored";
    case order_outcome::accepted:                  return "accepted";
    case order_outcome::rejected:                  return "rejected";
    case order_outcome::replaced:                  return "replaced";
    case order_outcome::replace_canceled_existing: return "replace_canceled_existing";
    case order_outcome::canceled:                  return "canceled";
    case order_outcome::modified:                  return "modified";
    case order_outcome::executed:                  return "executed";
    case order_outcome::count_:                    break;
  }
  DFR_UNREACHABLE("unnamed order outcome");
}

struct order_entry_options {
  // Whether cross orders are currently in the late period, during which they cannot be canceled. This
  // is the condition §2.2 gives for a replace being *rejected* rather than turned into a cancel, and
  // without it the Reject path(and the token consumption that goes with it) is unreachable.
  bool late_cross_period{false};

  // The firm to stamp on an order whose Firm field was left blank, as §2.1 describes.
  std::string_view default_firm{"DFLT"};
};

struct order_entry_stats {
  std::uint64_t entered{0};
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
  std::uint64_t replaced{0};
  std::uint64_t canceled{0};
  std::uint64_t executed{0};
  std::uint64_t ignored_duplicate_token{0};
  std::uint64_t ignored_unknown_order{0};
  std::uint64_t shares_executed{0};
  std::uint64_t shares_canceled{0};
  std::uint64_t modified_or_none{0};

  [[nodiscard]] friend constexpr bool operator==(const order_entry_stats&,
                                                 const order_entry_stats&) = default;
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_ORDER_OUTCOME_HPP

// OUCH 4.2 field values, split by whether the specification closes the set.
//
// Two kinds of enumerated field, and treating them alike is a bug
// -------------------------------------------------------------
// Some fields have a fixed set: Buy/Sell is B, S, T or E; Order State is L or D; a System Event is S
// or E. A value outside those is corruption, and reporting it is right.
//
// Others are explicitly open. Of the cancel reasons and the reject reasons the specification says:
// *"Clients should anticipate additions to this list and thus support all capital letters of the
// English alphabet."* NASDAQ has in fact added to both repeatedly: the revision history lists cancel
// reasons E, X, H, K, F, G and reject reasons W, o, u, q, Q arriving over ten years. A client that
// validated those against a fixed list would start rejecting messages the day the exchange shipped a
// new code, and it would reject them *at the moment something unusual was happening to its orders*.
//
// So the closed sets are `enum class` with validity checks, and the open ones are a raw byte with a
// name lookup that is allowed to answer "unknown". The type system says which is which.

#ifndef DFR_WIRE_OUCH_ENUMS_HPP
#define DFR_WIRE_OUCH_ENUMS_HPP

#include <dfr/core/assert.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace wire::ouch {

// ---------------------------------------------------------------------------
// Closed sets
// ---------------------------------------------------------------------------

enum class side : std::uint8_t {
  buy = 'B',
  sell = 'S',
  // "sell short, client affirms ability to borrow securities in good deliverable form for delivery
  // within three business days": a distinct value, not a flag on sell, and Modify Order restricts
  // which transitions between the short variants are allowed.
  sell_short = 'T',
  sell_short_exempt = 'E',
};

[[nodiscard]] constexpr bool is_known(side value) noexcept {
  switch (value) {
    case side::buy:
    case side::sell:
    case side::sell_short:
    case side::sell_short_exempt:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view name_of(side value) noexcept {
  switch (value) {
    case side::buy:               return "buy";
    case side::sell:              return "sell";
    case side::sell_short:        return "sell_short";
    case side::sell_short_exempt: return "sell_short_exempt";
  }
  DFR_UNREACHABLE("unnamed side");
}

// §3.4: the transitions Modify Order permits. Buy cannot become a sell and a sell cannot become a buy:
// only the three short variants may be exchanged for one another.
[[nodiscard]] constexpr bool is_permitted_modify_transition(side from,
                                                           side to) noexcept {
  if (from == to) {
    return false;  // a modify that changes nothing is not one of the listed transitions
  }
  const auto shortable = [](side value) {
    return value == side::sell || value == side::sell_short ||
           value == side::sell_short_exempt;
  };
  return shortable(from) && shortable(to);
}

// §3.3: "L" = Order Live, "D" = Order Dead. Dead on an Accepted message means the order was accepted
// and immediately canceled, and no further messages will arrive for it, which is a different thing
// from a rejection and must not be reported as one.
enum class order_state : std::uint8_t {
  live = 'L',
  dead = 'D',
};

[[nodiscard]] constexpr bool is_known(order_state value) noexcept {
  return value == order_state::live || value == order_state::dead;
}

[[nodiscard]] constexpr std::string_view name_of(order_state value) noexcept {
  switch (value) {
    case order_state::live: return "live";
    case order_state::dead: return "dead";
  }
  DFR_UNREACHABLE("unnamed order state");
}

// §3.2. Start of Day is always the first message of the session; after End of Day no new orders or
// replaces are accepted, but Broken Trade and Canceled messages may still arrive.
enum class event_code : std::uint8_t {
  start_of_day = 'S',
  end_of_day = 'E',
};

[[nodiscard]] constexpr bool is_known(event_code value) noexcept {
  return value == event_code::start_of_day || value == event_code::end_of_day;
}

[[nodiscard]] constexpr std::string_view name_of(event_code value) noexcept {
  switch (value) {
    case event_code::start_of_day: return "start_of_day";
    case event_code::end_of_day:   return "end_of_day";
  }
  DFR_UNREACHABLE("unnamed event code");
}

// §2.1: "Values other than 'A', 'P', or 'R' will be converted to 'O' = Other". The conversion is the
// exchange's, so a client that rejected an unexpected value would refuse a message the exchange
// considers well formed.
enum class capacity : std::uint8_t {
  agency = 'A',
  principal = 'P',
  riskless = 'R',
  other = 'O',
};

[[nodiscard]] constexpr capacity capacity_from_wire(std::uint8_t byte) noexcept {
  switch (static_cast<capacity>(byte)) {
    case capacity::agency:
    case capacity::principal:
    case capacity::riskless:
      return static_cast<capacity>(byte);
    case capacity::other:
      break;
  }
  return capacity::other;
}

enum class sweep_eligibility : std::uint8_t {
  eligible = 'Y',
  not_eligible = 'N',
  trade_at_sweep = 'y',  // lowercase, and distinct: "Trade-at Intermarket Sweep Order"
};

[[nodiscard]] constexpr bool is_known(sweep_eligibility value) noexcept {
  switch (value) {
    case sweep_eligibility::eligible:
    case sweep_eligibility::not_eligible:
    case sweep_eligibility::trade_at_sweep:
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Open sets: a raw byte plus a lookup that may answer "unknown"
// ---------------------------------------------------------------------------

// A reason code the exchange may extend. Held as the byte it was, so a client can log and act on a
// code this build has never heard of instead of discarding the message that carried it.
class reason_code {
 public:
  constexpr reason_code() noexcept = default;
  explicit constexpr reason_code(std::uint8_t byte) noexcept : byte_(byte) {}

  [[nodiscard]] constexpr std::uint8_t byte() const noexcept { return byte_; }
  [[nodiscard]] constexpr char as_char() const noexcept {
    return static_cast<char>(byte_);
  }

  [[nodiscard]] friend constexpr bool operator==(reason_code, reason_code) = default;

 private:
  std::uint8_t byte_{0};
};

// §3.5.1 Cancel Order Reasons, as of the October 2025 revision. The list has grown five times since
// 2015, which is why an unrecognised code returns a name rather than an error.
[[nodiscard]] constexpr std::string_view name_of_cancel_reason(
    reason_code reason) noexcept {
  switch (reason.as_char()) {
    case 'U': return "user_requested";
    case 'I': return "immediate_or_cancel";
    case 'T': return "timeout";
    case 'S': return "supervisory";
    case 'D': return "regulatory_restriction";
    case 'Q': return "self_match_prevention";
    case 'Z': return "system_cancel";
    case 'C': return "cross_canceled";
    case 'K': return "market_collars";
    case 'H': return "halted";
    case 'X': return "open_protection";
    case 'E': return "closed";
    case 'F': return "post_only_price_slide";
    case 'G': return "post_only_contra_displayed";
    default:  return "unknown";
  }
}

// §3.8.1 Broken Trade Reasons.
[[nodiscard]] constexpr std::string_view name_of_broken_reason(
    reason_code reason) noexcept {
  switch (reason.as_char()) {
    case 'E': return "erroneous";
    case 'C': return "consent";
    case 'S': return "supervisory";
    case 'X': return "external";
    default:  return "unknown";
  }
}

// §3.10.1 Rejected Order Reasons: the longest and most frequently extended list in the protocol.
// Only the ones a simulator can plausibly produce are named; the rest answer "unknown" by design.
[[nodiscard]] constexpr std::string_view name_of_reject_reason(
    reason_code reason) noexcept {
  switch (reason.as_char()) {
    case 'C': return "nasdaq_closed";
    case 'H': return "halted";
    case 'S': return "invalid_stock";
    case 'X': return "invalid_price";
    case 'N': return "invalid_minimum_quantity";
    case 'D': return "invalid_display_type";
    case 'O': return "other";
    case 'T': return "test_mode";
    case 'Z': return "shares_exceed_safety_threshold";
    case 'e': return "risk_fat_finger";
    case 'm': return "risk_max_shares";
    case 'n': return "risk_max_notional";
    default:  return "unknown";
  }
}

// A liquidity flag. Thirty-odd values in the current revision and more added most years, so it is a
// byte with a lookup rather than an enumeration, and the one distinction a caller usually needs is
// simply whether the execution added or removed liquidity, which is what the two helpers answer.
[[nodiscard]] constexpr bool added_liquidity(reason_code flag) noexcept {
  switch (flag.as_char()) {
    case 'A': case 'J': case 'W': case 'k': case '7': case '8':
    case 'e': case 'f': case 'j': case '4': case '5': case 'g':
    case 'u': case '2': case '9':
      return true;
    default:
      return false;
  }
}

[[nodiscard]] constexpr bool removed_liquidity(reason_code flag) noexcept {
  switch (flag.as_char()) {
    case 'R': case 'm': case 'd': case 'r': case 't': case '6': case '3':
      return true;
    default:
      return false;
  }
}

}  // namespace wire::ouch
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_OUCH_ENUMS_HPP

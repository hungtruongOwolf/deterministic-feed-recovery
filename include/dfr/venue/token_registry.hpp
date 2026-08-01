// Day-unique order tokens, and the asymmetry in when one is used up.
//
// The token is the protocol's idempotency key
// -----------------------------------------
// OUCH §2 permits a client to re-send any inbound message: *"This gives the client the ability to
// re-send any Inbound message if it is uncertain whether NASDAQ received it in the case of a connection
// loss or an application error."* That is safe only because the exchange recognises a token it has
// already seen and ignores the message. So this registry is what makes the whole failover model work,
// and getting it wrong either loses an order or duplicates one.
//
// Consumed and not consumed, which the specification distinguishes and it is easy to miss
// -------------------------------------------------------------------------------------
// §2.2 lists what happens to a *replacement* token in each outcome:
//
//   - existing order not live, or replacement token already used → silently ignored;
//   - live but the replace details are invalid → a Canceled Message removes the existing order, and
//     "The replacement Order Token will not be consumed, and may be reused in this case";
//   - live but the existing order cannot be canceled → a Reject Message, and "The Reject Message
//     consumes the replacement Order Token, so the replacement Order Token may not be reused";
//   - live and replaceable → Replaced.
//
// So a rejection burns the token and an invalid-details cancel does not. An implementation that got
// that backwards would either refuse a client's legitimate retry or accept a duplicate as a new order.
// §3.10 adds the same rule for a plain rejection: "The Order Token of a Rejected Message cannot be
// re-used."

#ifndef DFR_VENUE_TOKEN_REGISTRY_HPP
#define DFR_VENUE_TOKEN_REGISTRY_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/ouch/token.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace venue {

// How many tokens one account may use in a day. Bounded like everything else: a registry that grew
// without limit would be the one place in the venue that allocated in the steady state.
inline constexpr std::size_t kMaxTokensPerDay = 4'096;

class token_registry {
 public:
  constexpr token_registry() noexcept = default;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool full() const noexcept {
    return count_ >= kMaxTokensPerDay;
  }

  [[nodiscard]] constexpr bool is_used(const wire::ouch::order_token& token) const noexcept {
    return find(token) < count_;
  }

  // Marks a token as used for the rest of the day.
  //
  // Returns `invalid_argument` for a token already present: the caller's cue to ignore the message
  // rather than to report an error to the client, since the protocol's answer to a duplicate is
  // silence.
  [[nodiscard]] constexpr result<void> consume(
      const wire::ouch::order_token& token) noexcept {
    if (is_used(token)) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    if (full()) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }
    // Kept sorted so a lookup is a binary search over a flat array: no map, no hashing, no allocation,
    // and the same decision docs/DESIGN.md §0 records for the recovery channel table.
    std::size_t at = count_;
    while (at > 0 && token < tokens_[at - 1]) {
      tokens_[at] = tokens_[at - 1];
      --at;
    }
    tokens_[at] = token;
    ++count_;
    return ok();
  }

  constexpr void clear() noexcept { count_ = 0; }

 private:
  [[nodiscard]] constexpr std::size_t find(
      const wire::ouch::order_token& token) const noexcept {
    std::size_t low = 0;
    std::size_t high = count_;
    while (low < high) {
      const std::size_t mid = low + (high - low) / 2;
      if (tokens_[mid] < token) {
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    return low < count_ && tokens_[low] == token ? low : count_;
  }

  std::array<wire::ouch::order_token, kMaxTokensPerDay> tokens_{};
  std::size_t count_{0};
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_TOKEN_REGISTRY_HPP

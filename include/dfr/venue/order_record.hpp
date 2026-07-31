// One order's state at the exchange, and the identity that has to hold over it.
//
// The accounting identity
// ---------------------
// `shares_liable == shares_executed + shares_open + shares_canceled`, at every moment, for every order
// in a replace chain. It is the order-side equivalent of the recovery accounting identity — every
// message delivered exactly once or recorded missing — and it is what makes a wrong reading of the
// Shares field detectable instead of merely expensive.
//
// Shares liable is cumulative across the chain, inclusive of prior executions, because that is what a
// Replace Order message means (OUCH §2.2). So the identity is stated over the *chain*, not over one
// order, and a replace carries the executed count forward rather than starting it again.

#ifndef DFR_VENUE_ORDER_RECORD_HPP
#define DFR_VENUE_ORDER_RECORD_HPP

#include <dfr/core/assert.hpp>
#include <dfr/wire/ouch/enums.hpp>
#include <dfr/wire/ouch/price.hpp>
#include <dfr/wire/ouch/token.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace venue {

namespace ouch = wire::ouch;

struct order_record {
  ouch::order_token token{};

  // The token this order replaced, or empty for the first order of a chain. Kept so a Replaced message
  // can name it and so a chain can be walked for the accounting identity.
  ouch::order_token previous_token{};

  ouch::side order_side{ouch::side::buy};

  // Owned, not a view. The inbound message's `stock` points into the caller's receive buffer, which the
  // next message overwrites — a venue holding that view would report whatever arrived most recently.
  std::array<char, 8> stock_bytes{};
  std::uint8_t stock_length{0};

  ouch::price limit{};
  std::uint32_t time_in_force{0};
  std::uint8_t display{'A'};
  ouch::capacity account_capacity{ouch::capacity::other};
  std::uint8_t cross_type{'N'};

  // Cumulative for the whole replace chain, inclusive of everything already executed.
  std::uint32_t shares_liable{0};
  std::uint32_t shares_executed{0};
  std::uint32_t shares_canceled{0};

  std::uint64_t reference_number{0};
  bool live{false};

  [[nodiscard]] constexpr std::string_view stock() const noexcept {
    return std::string_view{stock_bytes.data(), stock_length};
  }

  constexpr void set_stock(std::string_view text) noexcept {
    DFR_ASSERT(text.size() <= stock_bytes.size(), "a stock symbol is eight bytes");
    stock_length = static_cast<std::uint8_t>(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      stock_bytes[i] = text[i];
    }
  }

  // What is still exposed on the book.
  [[nodiscard]] constexpr std::uint32_t shares_open() const noexcept {
    const std::uint32_t accounted = shares_executed + shares_canceled;
    return shares_liable > accounted ? shares_liable - accounted : 0;
  }

  // The identity, as a function so it can be asserted after every mutation rather than argued about
  // once per call site.
  [[nodiscard]] constexpr bool accounts() const noexcept {
    return shares_executed + shares_canceled + shares_open() == shares_liable;
  }

  // A cross order in the late period cannot be canceled, which is the condition OUCH §2.2 gives for a
  // replace being rejected outright rather than turned into a cancel. Modelled here because without it
  // the Reject path — and the token consumption that goes with it — is unreachable.
  [[nodiscard]] constexpr bool is_cross() const noexcept { return cross_type != 'N'; }
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_ORDER_RECORD_HPP

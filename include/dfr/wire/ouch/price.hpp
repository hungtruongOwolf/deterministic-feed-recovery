// An OUCH price: a 32-bit integer in ten-thousandths of a dollar, with market orders encoded in it.
//
// From OUCH 4.2 §1.2: "prices are in fixed point format with 6 whole number places followed by 4
// decimal digits". So $12.34 is 123,400 and there is no floating point anywhere near the wire, which
// is the entire reason this is a type rather than a `std::uint32_t`.
//
// Market orders live at the top of the same field
// ----------------------------------------------
// The maximum *limit* price is $199,999.9900 (1,999,999,900, or 0x7735939C in the specification's
// own words). A market order is $214,748.3647 (0x7FFFFFFF), and the specification adds that "orders
// entered with a price of $200,000.00 or the max integer value will also be treated as market
// orders".
//
// So the boundary is not the sentinel: everything from $200,000.00 upward means market, and an
// implementation that compared only against 0x7FFFFFFF would send a $250,000 limit order and have it
// silently executed at any price. That is a real money bug hiding in an equality test, so the
// comparison here is a threshold and the sentinel is only what we *write*.

#ifndef DFR_WIRE_OUCH_PRICE_HPP
#define DFR_WIRE_OUCH_PRICE_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>

#include <cstdint>

namespace dfr::inline v1::wire::ouch {

// Ten-thousandths of a dollar per unit, so the scale is 10^4 and a whole dollar is 10,000.
inline constexpr std::uint32_t kPriceScale = 10'000;

// $199,999.9900: the highest price that still means a limit.
inline constexpr std::uint32_t kMaxLimitRaw = 1'999'999'900;

// $200,000.00: the lowest value that means market. The threshold, not the sentinel.
inline constexpr std::uint32_t kMarketThresholdRaw = 2'000'000'000;

// $214,748.3647: what the specification names as *the* market price, and what we write.
inline constexpr std::uint32_t kMarketRaw = 0x7FFF'FFFFU;

class price {
 public:
  constexpr price() noexcept = default;

  // Deliberately explicit and named for the unit. `price{1234}` would read as $1,234 to half the
  // people who saw it and as $0.1234 to the other half.
  [[nodiscard]] static constexpr price from_raw(std::uint32_t raw) noexcept {
    return price{raw};
  }

  // Refuses a value that would land in the market range, so a caller cannot build one by accident and
  // discover it at the exchange.
  [[nodiscard]] static constexpr result<price> from_dollars_and_ten_thousandths(
      std::uint32_t dollars, std::uint32_t ten_thousandths) noexcept {
    if (ten_thousandths >= kPriceScale) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    // Checked before multiplying: 6 whole places means dollars below 1,000,000, and the product of a
    // larger value would wrap into a plausible small price.
    if (dollars > kMaxLimitRaw / kPriceScale) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    const std::uint32_t raw = dollars * kPriceScale + ten_thousandths;
    if (raw > kMaxLimitRaw) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    return price{raw};
  }

  [[nodiscard]] static constexpr price market() noexcept { return price{kMarketRaw}; }

  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return raw_; }

  // The threshold comparison, not an equality against the sentinel. See the note at the top.
  [[nodiscard]] constexpr bool is_market() const noexcept {
    return raw_ >= kMarketThresholdRaw;
  }

  // Whole dollars and the fractional remainder, for a report. Asserted rather than returning
  // something for a market order, because "market" has no dollar value and printing $214,748.36 for
  // it would be worse than refusing.
  [[nodiscard]] constexpr std::uint32_t dollars() const noexcept {
    DFR_ASSERT(!is_market(), "a market order has no dollar price");
    return raw_ / kPriceScale;
  }
  [[nodiscard]] constexpr std::uint32_t ten_thousandths() const noexcept {
    DFR_ASSERT(!is_market(), "a market order has no dollar price");
    return raw_ % kPriceScale;
  }

  // Whether this is a price the exchange will accept as a limit. A value between the limit maximum and
  // the market threshold is neither: the specification leaves that band undefined, so it is reported
  // rather than guessed at.
  [[nodiscard]] constexpr bool is_valid_limit() const noexcept {
    return raw_ <= kMaxLimitRaw;
  }

  [[nodiscard]] friend constexpr bool operator==(price, price) = default;
  [[nodiscard]] friend constexpr auto operator<=>(price, price) = default;

 private:
  explicit constexpr price(std::uint32_t raw) noexcept : raw_(raw) {}

  std::uint32_t raw_{0};
};

static_assert(kMaxLimitRaw == 0x7735'939CU,
              "the specification states the maximum limit price as 7735939C hex");
static_assert(price::market().is_market());
static_assert(price::from_raw(kMarketThresholdRaw).is_market(),
              "$200,000.00 is market, and comparing against the sentinel alone would miss it");
static_assert(!price::from_raw(kMaxLimitRaw).is_market());

}  // namespace dfr::inline v1::wire::ouch
#endif  // DFR_WIRE_OUCH_PRICE_HPP

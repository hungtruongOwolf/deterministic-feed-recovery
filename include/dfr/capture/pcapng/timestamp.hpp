// pcapng timestamp resolution.
//
// An Enhanced Packet Block carries a 64-bit tick count split across two 32-bit
// fields. What one tick *means* is not in the packet block at all — it comes
// from the `if_tsresol` option of the interface the packet arrived on, and it
// defaults to microseconds when that option is absent.
//
// So a reader that assumes microseconds is right for most files and silently
// wrong by a factor of a thousand for the nanosecond ones, with no symptom until
// two captures are compared. That is the same trap as classic pcap's magic, one
// layer further from the data — and worse, because here the answer is per
// interface rather than per file.
//
// Its own file because the arithmetic is worth testing in isolation: the binary
// form needs a 128-bit intermediate, and getting it wrong produces plausible
// timestamps.

#ifndef DFR_CAPTURE_PCAPNG_TIMESTAMP_HPP
#define DFR_CAPTURE_PCAPNG_TIMESTAMP_HPP

#include <dfr/core/detail/wide_multiply.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace capture::pcapng {

// The `if_tsresol` option value, stored raw.
//
// One byte. If the high bit is clear the low seven bits are a power of ten; if
// set, a power of two. Keeping it raw rather than pre-converting to a multiplier
// means the two forms stay distinguishable, which matters because only one of
// them is exact.
class tick_resolution {
 public:
  static constexpr std::uint8_t kDefaultRaw = 6;  // microseconds
  static constexpr std::uint8_t kBinaryFlag = 0x80;

  constexpr tick_resolution() noexcept = default;
  explicit constexpr tick_resolution(std::uint8_t raw) noexcept : raw_(raw) {}

  [[nodiscard]] constexpr std::uint8_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool binary() const noexcept {
    return (raw_ & kBinaryFlag) != 0;
  }
  [[nodiscard]] constexpr std::uint8_t exponent() const noexcept {
    return static_cast<std::uint8_t>(raw_ & 0x7F);
  }

  // Whether ticks convert to nanoseconds without loss.
  //
  // Exposed rather than hidden because a caller comparing timestamps across
  // captures should know when it is looking at rounded values. Decimal
  // resolutions coarser than a nanosecond are exact; finer ones and every binary
  // resolution beyond 2^-30 are not.
  [[nodiscard]] constexpr bool exact_in_nanoseconds() const noexcept {
    return !binary() && exponent() <= 9;
  }

  // Ticks to nanoseconds.
  //
  // Integer arithmetic throughout, with a 128-bit intermediate for the binary
  // case. No floating point: a double would reintroduce exactly the determinism
  // leaks core/rng.hpp exists to avoid, and a timestamp that differs in its last
  // digit between two runs of the same tool is indistinguishable from a real
  // discrepancy in the capture.
  [[nodiscard]] constexpr std::uint64_t to_nanoseconds(
      std::uint64_t ticks) const noexcept {
    if (!binary()) {
      const std::uint8_t power = exponent();
      if (power <= 9) {
        // Coarser than a nanosecond: scale up, exactly.
        return ticks * pow10(static_cast<std::uint8_t>(9 - power));
      }
      // Finer than a nanosecond. Truncates, and exact_in_nanoseconds() says so.
      return ticks / pow10(static_cast<std::uint8_t>(power - 9));
    }

    // Binary: one tick is 2^-n seconds, so nanoseconds are ticks * 1e9 / 2^n.
    // Doing the multiply first keeps the precision, which is why it needs 128
    // bits — ticks alone can approach 2^64.
    const auto product = detail::wide_multiply(ticks, 1'000'000'000ULL);
    const std::uint8_t shift = exponent();
    if (shift == 0) {
      return product.low;
    }
    if (shift >= 64) {
      // 2^-64 of a second and finer. The result is below one nanosecond, so it
      // rounds to the high word alone; a real capture will never do this.
      return product.high >> (shift - 64);
    }
    return (product.high << (64 - shift)) | (product.low >> shift);
  }

  [[nodiscard]] friend constexpr bool operator==(tick_resolution,
                                                 tick_resolution) = default;

 private:
  [[nodiscard]] static constexpr std::uint64_t pow10(std::uint8_t power) noexcept {
    std::uint64_t out = 1;
    for (std::uint8_t i = 0; i < power; ++i) {
      out *= 10;
    }
    return out;
  }

  std::uint8_t raw_{kDefaultRaw};
};

// The default really is microseconds, not nanoseconds. Pinned because the wrong
// default is off by a thousand on every file that omits the option, which is
// most of them.
static_assert(tick_resolution{}.to_nanoseconds(1) == 1'000);
static_assert(tick_resolution{9}.to_nanoseconds(1) == 1);
static_assert(tick_resolution{6}.to_nanoseconds(1'500'000) == 1'500'000'000);
static_assert(tick_resolution{3}.to_nanoseconds(1) == 1'000'000);
static_assert(tick_resolution{0}.to_nanoseconds(1) == 1'000'000'000);
static_assert(tick_resolution{}.exact_in_nanoseconds());
static_assert(!tick_resolution{12}.exact_in_nanoseconds());

}  // namespace capture::pcapng
}  // namespace dfr::inline v1

#endif  // DFR_CAPTURE_PCAPNG_TIMESTAMP_HPP

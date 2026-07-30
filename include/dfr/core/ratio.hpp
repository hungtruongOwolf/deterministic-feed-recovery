// A probability, as an exact integer ratio.
//
// Not a double, and not a percentage. A double would reintroduce the floating
// point determinism leaks the generator exists to avoid, and a percentage cannot
// express the small probabilities a fault injector needs — FoundationDB's
// BUGGIFY sites fire at rates that matter down to fractions of a per cent.
//
// This is TigerBeetle's stdx.PRNG design: no floating point anywhere in the API.

#ifndef DFR_CORE_RATIO_HPP
#define DFR_CORE_RATIO_HPP

#include <cstdint>

namespace dfr::inline v1 {

// small probabilities a fault injector needs — FoundationDB's BUGGIFY sites
// fire at rates that matter down to fractions of a per cent.
struct ratio {
  std::uint64_t numerator{0};
  std::uint64_t denominator{1};

  [[nodiscard]] friend constexpr bool operator==(ratio, ratio) = default;
};

[[nodiscard]] constexpr ratio percent(std::uint64_t n) noexcept {
  return ratio{n, 100};
}

inline constexpr ratio kNever{0, 1};
inline constexpr ratio kAlways{1, 1};

}  // namespace dfr::inline v1

#endif  // DFR_CORE_RATIO_HPP

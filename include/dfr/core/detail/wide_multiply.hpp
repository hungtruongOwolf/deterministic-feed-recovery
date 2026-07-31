// The full 128-bit product of two 64-bit values.
//
// Its own file because it is tested directly rather than only through prng: a
// wrong portable fallback would silently bias every bounded draw, and only MSVC
// would ever execute it.

#ifndef DFR_CORE_DETAIL_WIDE_MULTIPLY_HPP
#define DFR_CORE_DETAIL_WIDE_MULTIPLY_HPP

#include <dfr/core/attributes.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace detail {

// The full 128-bit product of two 64-bit values.
struct wide_product {
  std::uint64_t high{0};
  std::uint64_t low{0};

  [[nodiscard]] friend constexpr bool operator==(wide_product,
                                                 wide_product) = default;
};

// Schoolbook 32x32 decomposition. Always compiled, even where a 128-bit type is
// available, for one reason: it is the path MSVC takes, a wrong version of it
// would silently bias every bounded draw, and nothing else would ever execute
// it. Compiling it unconditionally lets a test compare the two implementations
// on whatever platform the test happens to run on.
[[nodiscard]] constexpr wide_product wide_multiply_portable(
    std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a_lo = a & 0xFFFF'FFFFULL;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & 0xFFFF'FFFFULL;
  const std::uint64_t b_hi = b >> 32;

  const std::uint64_t ll = a_lo * b_lo;
  const std::uint64_t lh = a_lo * b_hi;
  const std::uint64_t hl = a_hi * b_lo;
  const std::uint64_t hh = a_hi * b_hi;

  const std::uint64_t mid =
      (ll >> 32) + (lh & 0xFFFF'FFFFULL) + (hl & 0xFFFF'FFFFULL);
  const std::uint64_t low = (ll & 0xFFFF'FFFFULL) | (mid << 32);
  const std::uint64_t high = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
  return wide_product{high, low};
}

[[nodiscard]] DFR_FLATTEN_INLINE constexpr wide_product wide_multiply(
    std::uint64_t a, std::uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
  // __extension__ silences -Wpedantic: there is no standard 128-bit type in
  // C++20 and unsigned __int128 is a GNU extension.
  __extension__ using u128 = unsigned __int128;
  const u128 product = static_cast<u128>(a) * static_cast<u128>(b);
  return wide_product{static_cast<std::uint64_t>(product >> 64),
                      static_cast<std::uint64_t>(product)};
#else
  return wide_multiply_portable(a, b);
#endif
}

}  // namespace detail

}  // namespace dfr::inline v1

#endif  // DFR_CORE_DETAIL_WIDE_MULTIPLY_HPP

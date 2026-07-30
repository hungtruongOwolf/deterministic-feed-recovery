// Byte-range vocabulary shared by the view types.
//
// Split out from packet_view.hpp so that a caller who only needs to say "a
// contiguous range of bytes" in a signature does not pull in the views, and so
// that the two native load/store helpers have one obvious home. See
// docs/STYLE.md section 1.10.

#ifndef DFR_CORE_BYTES_HPP
#define DFR_CORE_BYTES_HPP

#include <dfr/core/attributes.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dfr::inline v1 {


// Any contiguous range of bytes, whatever its element spelling. Wire buffers
// arrive as std::byte, char, unsigned char and uint8_t depending on which
// capture library produced them, and requiring the caller to cast is friction
// that invites a wrong cast.
template <typename T>
concept byte_like = std::same_as<std::remove_cv_t<T>, std::byte> ||
                    std::same_as<std::remove_cv_t<T>, char> ||
                    std::same_as<std::remove_cv_t<T>, unsigned char> ||
                    std::same_as<std::remove_cv_t<T>, signed char>;

namespace detail {

// Assemble an unsigned integer from `sizeof(T)` bytes at `data`, native order.
//
// Free functions rather than private member templates. A private member
// template defined below the accessors that call it has no definition available
// at the point of a constant evaluation, so `be32_at` silently stops being
// usable in a static_assert. Hoisting them out removes the ordering question.
//
// Byte-at-a-time rather than memcpy so these stay constexpr; std::memcpy is not
// a constant expression. Compilers fold this to a single unaligned load, plus a
// byte reverse where one is needed, from -O1 upwards.
template <std::unsigned_integral T>
[[nodiscard]] DFR_FLATTEN_INLINE constexpr T load_native(
    const std::byte* data) noexcept {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value = static_cast<T>(value | (static_cast<T>(data[i]) << (8 * i)));
  }
  return value;
}

template <std::unsigned_integral T>
DFR_FLATTEN_INLINE constexpr void store_native(std::byte* data,
                                               T value) noexcept {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    data[i] = static_cast<std::byte>((value >> (8 * i)) & static_cast<T>(0xFF));
  }
}

}  // namespace detail

}  // namespace dfr::inline v1

#endif  // DFR_CORE_BYTES_HPP

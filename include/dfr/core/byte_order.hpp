// Byte-order conversion.
//
// Both endiannesses are needed, and not as a portability afterthought: the two
// transports this library targets disagree. MoldUDP64 is big-endian throughout
// (NASDAQ MoldUDP64 spec), while IEX-TP is little-endian (IEX Transport
// Specification). A single `ntoh`-style helper would make one of them read
// wrong at every call site.
//
// So the endianness is part of every accessor's *name*, never a parameter and
// never implied by the platform. This is Aeron's lesson applied to byte order
// rather than to memory ordering: getInt32 and getInt32Volatile are distinct
// names precisely so that the property is greppable.

#ifndef DFR_CORE_BYTE_ORDER_HPP
#define DFR_CORE_BYTE_ORDER_HPP

#include <dfr/core/attributes.hpp>

#include <bit>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace dfr::inline v1 {

// std::byteswap is C++23. This is the same operation, spelled for C++20.
//
// The manual arithmetic is deliberately not hand-unrolled per width: every
// compiler recognises this shape and emits a single instruction (rev/bswap).
// Verified on the toolchains in CI rather than assumed.
template <std::unsigned_integral T>
[[nodiscard]] DFR_FLATTEN_INLINE constexpr T byteswap(T value) noexcept {
  if constexpr (sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    return static_cast<T>((value << 8) | (value >> 8));
  } else if constexpr (sizeof(T) == 4) {
    return static_cast<T>(((value & 0x0000'00FFU) << 24) |
                          ((value & 0x0000'FF00U) << 8) |
                          ((value & 0x00FF'0000U) >> 8) |
                          ((value & 0xFF00'0000U) >> 24));
  } else {
    static_assert(sizeof(T) == 8, "byteswap supports 1, 2, 4 and 8 byte types");
    return static_cast<T>(((value & 0x0000'0000'0000'00FFULL) << 56) |
                          ((value & 0x0000'0000'0000'FF00ULL) << 40) |
                          ((value & 0x0000'0000'00FF'0000ULL) << 24) |
                          ((value & 0x0000'0000'FF00'0000ULL) << 8) |
                          ((value & 0x0000'00FF'0000'0000ULL) >> 8) |
                          ((value & 0x0000'FF00'0000'0000ULL) >> 24) |
                          ((value & 0x00FF'0000'0000'0000ULL) >> 40) |
                          ((value & 0xFF00'0000'0000'0000ULL) >> 56));
  }
}

// Mixed-endian platforms exist but none that this library targets. Failing at
// compile time is better than silently producing one of the two possible wrong
// answers.
static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big,
              "dfr requires a pure little- or big-endian platform");

inline constexpr bool kNativeIsLittleEndian =
    std::endian::native == std::endian::little;

// Host value from a big-endian wire value, and back. Named for the wire order,
// not for the direction, so that a call site reads as a statement about the
// protocol: `from_big_endian(raw)` says the bytes on the wire were big-endian.
template <std::unsigned_integral T>
[[nodiscard]] DFR_FLATTEN_INLINE constexpr T from_big_endian(T wire) noexcept {
  if constexpr (kNativeIsLittleEndian) {
    return byteswap(wire);
  } else {
    return wire;
  }
}

template <std::unsigned_integral T>
[[nodiscard]] DFR_FLATTEN_INLINE constexpr T to_big_endian(T host) noexcept {
  return from_big_endian(host);  // the conversion is its own inverse
}

template <std::unsigned_integral T>
[[nodiscard]] DFR_FLATTEN_INLINE constexpr T from_little_endian(
    T wire) noexcept {
  if constexpr (kNativeIsLittleEndian) {
    return wire;
  } else {
    return byteswap(wire);
  }
}

template <std::unsigned_integral T>
[[nodiscard]] DFR_FLATTEN_INLINE constexpr T to_little_endian(T host) noexcept {
  return from_little_endian(host);
}

}  // namespace dfr::inline v1

#endif  // DFR_CORE_BYTE_ORDER_HPP

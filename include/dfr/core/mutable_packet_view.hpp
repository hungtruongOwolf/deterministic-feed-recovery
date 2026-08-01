// mutable_packet_view: the write side.
//
// Separate from packet_view because the read path and the write path have
// different callers: everything decodes, only dfr::chaos and dfr::venue encode.
// A reader tracing a decode bug should never have to scroll past the mutators.

#ifndef DFR_CORE_MUTABLE_PACKET_VIEW_HPP
#define DFR_CORE_MUTABLE_PACKET_VIEW_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/byte_order.hpp>
#include <dfr/core/bytes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace dfr::inline v1 {

// ---------------------------------------------------------------------------
// mutable_packet_view
//
// The write side, used by dfr::chaos to corrupt a datagram in place. A separate
// type rather than a template parameter, so that a function signature says
// whether it mutates, and so that construction from a const buffer cannot
// compile.
//
// Conversion to packet_view is implicit; the reverse does not exist.
// ---------------------------------------------------------------------------

class DFR_VIEW mutable_packet_view {
 public:
  using size_type = std::size_t;

  constexpr mutable_packet_view() noexcept = default;

  // Explicit, unlike packet_view's constructor. Abseil's rule again: obtaining
  // a window you can write through should be visible at the call site.
  explicit constexpr mutable_packet_view(std::byte* data DFR_LIFETIME_BOUND,
                                         size_type size) noexcept
      : data_(data), size_(size) {
    DFR_ASSERT(data != nullptr || size == 0,
               "a null pointer with a non-zero size is never a valid view");
  }

  template <byte_like Byte>
    requires(!std::is_const_v<Byte> &&
             !std::same_as<std::remove_cv_t<Byte>, std::byte>)
  explicit mutable_packet_view(Byte* data DFR_LIFETIME_BOUND,
                               size_type size) noexcept
      : data_(reinterpret_cast<std::byte*>(data)), size_(size) {
    DFR_ASSERT(data != nullptr || size == 0,
               "a null pointer with a non-zero size is never a valid view");
  }

  template <std::size_t Extent>
  explicit constexpr mutable_packet_view(
      std::span<std::byte, Extent> bytes DFR_LIFETIME_BOUND) noexcept
      : mutable_packet_view(bytes.data(), bytes.size()) {}

  template <byte_like Byte, std::size_t Extent>
    requires(!std::is_const_v<Byte> &&
             !std::same_as<std::remove_cv_t<Byte>, std::byte>)
  explicit mutable_packet_view(
      std::span<Byte, Extent> bytes DFR_LIFETIME_BOUND) noexcept
      : mutable_packet_view(bytes.data(), bytes.size()) {}

  // Implicit narrowing to the read-only view, so every observer written against
  // packet_view works unchanged.
  [[nodiscard]] constexpr operator packet_view() const noexcept {
    return packet_view{data_, size_};
  }
  [[nodiscard]] constexpr packet_view as_const() const noexcept {
    return packet_view{data_, size_};
  }

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::byte* data() const noexcept {
    return data_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr size_type size() const noexcept {
    return size_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool empty() const noexcept {
    return size_ == 0;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool contains(
      size_type offset, size_type length) const noexcept {
    return offset <= size_ && length <= size_ - offset;
  }

  [[nodiscard]] constexpr std::span<std::byte> bytes() const noexcept {
    return {data_, size_};
  }

  [[nodiscard]] constexpr result<mutable_packet_view> subview(
      size_type offset, size_type length) const noexcept {
    if (!contains(offset, length)) DFR_UNLIKELY {
      return error::block_overruns_datagram;
    }
    return mutable_packet_view{data_ + offset, length};
  }

  // ---- writes ----------------------------------------------------------

  DFR_FLATTEN_INLINE constexpr void put_u8_at(size_type offset,
                                              std::uint8_t value) const noexcept {
    DFR_ASSERT(contains(offset, 1), "put_u8_at past the end of the view");
    data_[offset] = static_cast<std::byte>(value);
  }

  DFR_FLATTEN_INLINE constexpr void put_be16_at(
      size_type offset, std::uint16_t value) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint16_t)),
               "integer write past the end of the view");
    detail::store_native<std::uint16_t>(data_ + offset, to_big_endian(value));
  }
  DFR_FLATTEN_INLINE constexpr void put_be32_at(
      size_type offset, std::uint32_t value) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint32_t)),
               "integer write past the end of the view");
    detail::store_native<std::uint32_t>(data_ + offset, to_big_endian(value));
  }
  DFR_FLATTEN_INLINE constexpr void put_be64_at(
      size_type offset, std::uint64_t value) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint64_t)),
               "integer write past the end of the view");
    detail::store_native<std::uint64_t>(data_ + offset, to_big_endian(value));
  }
  DFR_FLATTEN_INLINE constexpr void put_le16_at(
      size_type offset, std::uint16_t value) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint16_t)),
               "integer write past the end of the view");
    detail::store_native<std::uint16_t>(data_ + offset, to_little_endian(value));
  }
  DFR_FLATTEN_INLINE constexpr void put_le32_at(
      size_type offset, std::uint32_t value) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint32_t)),
               "integer write past the end of the view");
    detail::store_native<std::uint32_t>(data_ + offset, to_little_endian(value));
  }
  DFR_FLATTEN_INLINE constexpr void put_le64_at(
      size_type offset, std::uint64_t value) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint64_t)),
               "integer write past the end of the view");
    detail::store_native<std::uint64_t>(data_ + offset, to_little_endian(value));
  }

  template <typename T>
    requires std::is_trivially_copyable_v<T>
  DFR_FLATTEN_INLINE void write_at(size_type offset,
                                   const T& value) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(T)), "write_at past the end of the view");
    std::memcpy(data_ + offset, &value, sizeof(T));
  }

  // Flip one bit. Named for what dfr::chaos uses it for: single-bit corruption
  // is the fault class that TigerBeetle's simulator missed for years because it
  // only ever corrupted whole sectors.
  DFR_FLATTEN_INLINE constexpr void flip_bit_at(size_type offset,
                                                unsigned bit) const noexcept {
    DFR_ASSERT(contains(offset, 1), "flip_bit_at past the end of the view");
    DFR_ASSERT(bit < 8, "bit index must be 0..7");
    data_[offset] ^= static_cast<std::byte>(1U << bit);
  }

 private:
  std::byte* data_{nullptr};
  size_type size_{0};
};


}  // namespace dfr::inline v1

#endif  // DFR_CORE_MUTABLE_PACKET_VIEW_HPP

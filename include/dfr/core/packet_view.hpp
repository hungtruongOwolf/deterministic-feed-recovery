// packet_view: a non-owning window onto received bytes.
//
// This header exists because of a specific, real defect. penberg/helix
// validates a MoldUDP64 header, then loops `MessageCount` times performing
// `reinterpret_cast<const moldudp64_message_block*>(p)` with no check that `p`
// or `p + message_length` is inside the datagram (moldudp64.hh:106-115). A
// faulty or hostile count walks off the buffer.
//
// The fix is not "bounds-check every field read", which would put a branch on
// every byte of the hot path. It is Aeron's flyweight model:
//
//   1. Slicing is *checked* and returns a result. Every narrowing of the window
//     (header off the front, one block out of the middle) goes through
//      subview(), which cannot produce a view that escapes its parent.
//   2. Reading a field inside an already-validated view is *asserted*. The
//      precondition is that the caller obtained the view from a successful
//      slice, and at that point the read is provably in range.
//
// So the safety lives at the slice, which happens once per structure, rather
// than at the read, which happens once per field. A caller that never calls an
// unchecked accessor without first slicing cannot reproduce the helix bug, and
// a caller that tries will trip an assertion in the dev and release builds.

#ifndef DFR_CORE_PACKET_VIEW_HPP
#define DFR_CORE_PACKET_VIEW_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/byte_order.hpp>
#include <dfr/core/bytes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace dfr::inline v1 {
// ---------------------------------------------------------------------------
// packet_view
// ---------------------------------------------------------------------------

class DFR_VIEW packet_view {
 public:
  using size_type = std::size_t;

  constexpr packet_view() noexcept = default;

  // Implicit from any contiguous byte range, following Abseil's rule that a
  // const view converts implicitly while a mutable one does not
  // (absl/types/span.h:123-128). Reading someone's buffer is safe; writing to
  // it should be spelled out.
  //
  // DFR_LIFETIME_BOUND is load-bearing here: it is what lets the compiler
  // diagnose `packet_view v = make_temporary_vector();`.
  // std::byte needs no conversion, so this overload is usable in a constant
  // expression. That matters more than it looks: it lets a wire-format test
  // build its expected values with the same accessors the decoder uses, with no
  // runtime fixture.
  constexpr packet_view(const std::byte* data DFR_LIFETIME_BOUND,
                        size_type size) noexcept
      : data_(data), size_(size) {
    DFR_ASSERT(data != nullptr || size == 0,
               "a null pointer with a non-zero size is never a valid view");
  }

  // Every other byte spelling needs a reinterpret_cast, which is forbidden in a
  // constant expression. Kept as a separate overload rather than casting
  // unconditionally, so that the std::byte path above stays constexpr.
  template <byte_like Byte>
    requires(!std::same_as<std::remove_cv_t<Byte>, std::byte>)
  packet_view(const Byte* data DFR_LIFETIME_BOUND, size_type size) noexcept
      : data_(reinterpret_cast<const std::byte*>(data)), size_(size) {
    DFR_ASSERT(data != nullptr || size == 0,
               "a null pointer with a non-zero size is never a valid view");
  }

  template <std::size_t Extent>
  constexpr packet_view(
      std::span<const std::byte, Extent> bytes DFR_LIFETIME_BOUND) noexcept
      : packet_view(bytes.data(), bytes.size()) {}

  template <byte_like Byte, std::size_t Extent>
    requires(!std::same_as<std::remove_cv_t<Byte>, std::byte>)
  packet_view(std::span<Byte, Extent> bytes DFR_LIFETIME_BOUND) noexcept
      : packet_view(bytes.data(), bytes.size()) {}

  // ---- observers --------------------------------------------------------

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr const std::byte* data()
      const noexcept {
    return data_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr size_type size() const noexcept {
    return size_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool empty() const noexcept {
    return size_ == 0;
  }

  [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept {
    return {data_, size_};
  }

  [[nodiscard]] constexpr const std::byte* begin() const noexcept {
    return data_;
  }
  [[nodiscard]] constexpr const std::byte* end() const noexcept {
    return data_ + size_;
  }

  // Whether [offset, offset + length) lies inside this view.
  //
  // Written so that no intermediate can overflow: `offset + length` on
  // attacker-controlled 64-bit values is exactly how a bounds check gets
  // bypassed, so the addition never happens.
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool contains(
      size_type offset, size_type length) const noexcept {
    return offset <= size_ && length <= size_ - offset;
  }

  // ---- checked slicing: the safety choke point --------------------------

  // A window onto [offset, offset + length). The returned view can never
  // escape this one, which is the property the whole design rests on.
  [[nodiscard]] constexpr result<packet_view> subview(
      size_type offset, size_type length) const noexcept {
    if (!contains(offset, length)) DFR_UNLIKELY {
      // Deliberately the framing code rather than invalid_argument: reaching
      // here means the *wire data* asked for more bytes than arrived, which is
      // this library's product, not a programmer error.
      return error::block_overruns_datagram;
    }
    return packet_view{data_ + offset, length};
  }

  // The first `length` bytes. Named prefix rather than first() so that it does
  // not read as an element accessor.
  [[nodiscard]] constexpr result<packet_view> prefix(
      size_type length) const noexcept {
    return subview(0, length);
  }

  // Everything from `offset` to the end.
  [[nodiscard]] constexpr result<packet_view> suffix(
      size_type offset) const noexcept {
    if (offset > size_) DFR_UNLIKELY {
      return error::block_overruns_datagram;
    }
    return packet_view{data_ + offset, size_ - offset};
  }

  // Advance this view past `length` bytes, in place. The idiom for walking a
  // sequence of blocks: slice one off the front, then advance.
  [[nodiscard]] constexpr result<void> consume(size_type length) noexcept {
    if (length > size_) DFR_UNLIKELY {
      return error::block_overruns_datagram;
    }
    data_ += length;
    size_ -= length;
    return ok();
  }

  // ---- unchecked reads -------------------------------------------------
  //
  // Precondition on every one of these: the offset and width are inside the
  // view. That holds by construction when the view came from a successful
  // subview() sized to the structure being decoded. The assertion is the net
  // for the case where it does not.
  //
  // Endianness is in the name, never inferred. See byte_order.hpp.

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint8_t u8_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, 1),
               "u8_at past the end of the view; slice with subview() first");
    return static_cast<std::uint8_t>(data_[offset]);
  }

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint16_t be16_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint16_t)),
               "integer read past the end of the view; slice with subview() "
               "first");
    return from_big_endian(detail::load_native<std::uint16_t>(data_ + offset));
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint32_t be32_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint32_t)),
               "integer read past the end of the view; slice with subview() "
               "first");
    return from_big_endian(detail::load_native<std::uint32_t>(data_ + offset));
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint64_t be64_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint64_t)),
               "integer read past the end of the view; slice with subview() "
               "first");
    return from_big_endian(detail::load_native<std::uint64_t>(data_ + offset));
  }

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint16_t le16_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint16_t)),
               "integer read past the end of the view; slice with subview() "
               "first");
    return from_little_endian(detail::load_native<std::uint16_t>(data_ + offset));
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint32_t le32_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint32_t)),
               "integer read past the end of the view; slice with subview() "
               "first");
    return from_little_endian(detail::load_native<std::uint32_t>(data_ + offset));
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint64_t le64_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(std::uint64_t)),
               "integer read past the end of the view; slice with subview() "
               "first");
    return from_little_endian(detail::load_native<std::uint64_t>(data_ + offset));
  }

  // Six-byte big-endian, widened to 64 bits.
  //
  // Not a general-purpose helper: ITCH 5.0 timestamps are exactly six bytes,
  // nanoseconds since midnight. Without this, every ITCH decoder open-codes the
  // same shift loop, and the ones that reach for be64_at read two bytes of the
  // following field.
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint64_t be48_at(
      size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, 6), "be48_at past the end of the view");
    std::uint64_t value = 0;
    for (size_type i = 0; i < 6; ++i) {
      value = (value << 8) | static_cast<std::uint64_t>(data_[offset + i]);
    }
    return value;
  }

  // A trivially copyable struct, by memcpy.
  //
  // memcpy rather than reinterpret_cast, always. A wire buffer has no alignment
  // guarantee, so casting a pointer to a type with alignment > 1 and reading
  // through it is undefined behaviour that happens to work on x86 and traps on
  // some ARM configurations. Every compiler turns this memcpy into the same
  // load a cast would have produced, so the safety is free.
  template <typename T>
    requires std::is_trivially_copyable_v<T>
  [[nodiscard]] DFR_FLATTEN_INLINE T read_at(size_type offset) const noexcept {
    DFR_ASSERT(contains(offset, sizeof(T)),
               "read_at past the end of the view; slice with subview() first");
    // Deliberately uninitialized: memcpy overwrites every byte on the next line, always, with no branch in
    // between. Zero-initializing first would be a write this function exists to avoid paying for, and
    // clang-tidy cannot see the memcpy that follows makes it redundant. T is also not guaranteed
    // default-constructible for every wire type this is instantiated with, so `T value{};` would risk a hard
    // compile error for a struct that has no reason to support one.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    T value;
    std::memcpy(&value, data_ + offset, sizeof(T));
    return value;
  }

  // ---- comparison -------------------------------------------------------

  // Compares contents, not addresses. Two views over identical bytes in
  // different buffers are equal, which is what a decoder test wants.
  [[nodiscard]] friend bool operator==(packet_view lhs,
                                       packet_view rhs) noexcept {
    if (lhs.size_ != rhs.size_) {
      return false;
    }
    if (lhs.size_ == 0) {
      return true;
    }
    return std::memcmp(lhs.data_, rhs.data_, lhs.size_) == 0;
  }

 private:
  const std::byte* data_{nullptr};
  size_type size_{0};
};

}  // namespace dfr::inline v1

#endif  // DFR_CORE_PACKET_VIEW_HPP

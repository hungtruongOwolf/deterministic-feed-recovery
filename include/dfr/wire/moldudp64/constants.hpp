// MoldUDP64 wire constants: field offsets, sizes, and the two sentinel counts.
//
// Separate so that a caller who only needs to size a buffer, or a test that only
// pins the layout, does not pull in the decoder.

#ifndef DFR_WIRE_MOLDUDP64_CONSTANTS_HPP
#define DFR_WIRE_MOLDUDP64_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::wire::moldudp64 {

// ---------------------------------------------------------------------------
// Wire constants
//
// Field offsets are named constants with accessors, rather than a packed
// struct. A packed struct over a received datagram requires either a
// reinterpret_cast at an arbitrary alignment: undefined behaviour that happens
// to work on x86, or a memcpy that then needs per-field byte swapping anyway.
//
// The static_asserts below give the same guarantee a `static_assert(sizeof(T))`
// plus `offsetof` per field would, and arguably a stronger one: they check that
// the fields *tile* the header exactly, with no gap and no overlap. Aeron,
// libtrading and SBE-generated code all pin sizes and none of them pin that.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kSessionOffset = 0;
inline constexpr std::size_t kSessionSize = 10;

inline constexpr std::size_t kSequenceOffset = kSessionOffset + kSessionSize;
inline constexpr std::size_t kSequenceSize = 8;

inline constexpr std::size_t kMessageCountOffset = kSequenceOffset + kSequenceSize;
inline constexpr std::size_t kMessageCountSize = 2;

inline constexpr std::size_t kHeaderSize =
    kMessageCountOffset + kMessageCountSize;

// Each message block is prefixed by its own length.
inline constexpr std::size_t kMessageLengthSize = 2;

static_assert(kHeaderSize == 20, "MoldUDP64 headers are 20 bytes");
static_assert(kSessionOffset == 0);
static_assert(kSequenceOffset == 10);
static_assert(kMessageCountOffset == 18);
static_assert(kSessionSize + kSequenceSize + kMessageCountSize == kHeaderSize,
              "the three header fields must tile the header exactly, with no "
              "gap and no overlap");

// Message Count values that mean something other than "this many messages".
inline constexpr std::uint16_t kHeartbeat = 0;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

// The most messages one retransmission request may ask for. From the spec; a
// request above this is rejected by the facility rather than truncated, so a
// client that does not clamp gets nothing back at all.
inline constexpr std::uint16_t kMaxMessagesPerRequest = 60'000;

static_assert(kMaxMessagesPerRequest < kEndOfSession,
              "a request count must never collide with the end-of-session "
              "sentinel");

}  // namespace dfr::inline v1::wire::moldudp64
#endif  // DFR_WIRE_MOLDUDP64_CONSTANTS_HPP

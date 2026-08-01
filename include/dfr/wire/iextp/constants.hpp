// IEX-TP wire constants: field offsets, sizes, and the known protocol ids.
//
// Every field is little-endian, the opposite of MoldUDP64. The static_asserts
// check that the ten fields tile the 40-byte header exactly, with no gap and no
// overlap.

#ifndef DFR_WIRE_IEXTP_CONSTANTS_HPP
#define DFR_WIRE_IEXTP_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::iextp {

// ---------------------------------------------------------------------------
// Wire constants
// ---------------------------------------------------------------------------

inline constexpr std::size_t kVersionOffset = 0;
inline constexpr std::size_t kVersionSize = 1;

inline constexpr std::size_t kReservedOffset = 1;
inline constexpr std::size_t kReservedSize = 1;

inline constexpr std::size_t kProtocolIdOffset = 2;
inline constexpr std::size_t kProtocolIdSize = 2;

inline constexpr std::size_t kChannelIdOffset = 4;
inline constexpr std::size_t kChannelIdSize = 4;

inline constexpr std::size_t kSessionIdOffset = 8;
inline constexpr std::size_t kSessionIdSize = 4;

inline constexpr std::size_t kPayloadLengthOffset = 12;
inline constexpr std::size_t kPayloadLengthSize = 2;

inline constexpr std::size_t kMessageCountOffset = 14;
inline constexpr std::size_t kMessageCountSize = 2;

inline constexpr std::size_t kStreamOffsetOffset = 16;
inline constexpr std::size_t kStreamOffsetSize = 8;

inline constexpr std::size_t kFirstSequenceOffset = 24;
inline constexpr std::size_t kFirstSequenceSize = 8;

inline constexpr std::size_t kSendTimeOffset = 32;
inline constexpr std::size_t kSendTimeSize = 8;

inline constexpr std::size_t kHeaderSize = 40;

// Each message block is prefixed by its own little-endian length.
inline constexpr std::size_t kMessageLengthSize = 2;

inline constexpr std::uint8_t kVersion = 0x01;

// The fields must tile the header exactly, with no gap and no overlap. Written
// as a sum rather than as a series of offset checks so that inserting a field
// without adjusting the total is a compile error.
static_assert(kVersionSize + kReservedSize + kProtocolIdSize + kChannelIdSize +
                      kSessionIdSize + kPayloadLengthSize + kMessageCountSize +
                      kStreamOffsetSize + kFirstSequenceSize + kSendTimeSize ==
                  kHeaderSize,
              "the ten header fields must tile the 40-byte header exactly");
static_assert(kVersionOffset + kVersionSize == kReservedOffset);
static_assert(kReservedOffset + kReservedSize == kProtocolIdOffset);
static_assert(kProtocolIdOffset + kProtocolIdSize == kChannelIdOffset);
static_assert(kChannelIdOffset + kChannelIdSize == kSessionIdOffset);
static_assert(kSessionIdOffset + kSessionIdSize == kPayloadLengthOffset);
static_assert(kPayloadLengthOffset + kPayloadLengthSize == kMessageCountOffset);
static_assert(kMessageCountOffset + kMessageCountSize == kStreamOffsetOffset);
static_assert(kStreamOffsetOffset + kStreamOffsetSize == kFirstSequenceOffset);
static_assert(kFirstSequenceOffset + kFirstSequenceSize == kSendTimeOffset);
static_assert(kSendTimeOffset + kSendTimeSize == kHeaderSize);

// Known message protocol identifiers, so a receiver can reject a channel it was
// not configured for rather than decoding TOPS as DEEP.
enum class protocol_id : std::uint16_t {
  deep = 0x8004,
  tops = 0x8003,
};

[[nodiscard]] constexpr std::string_view to_string(protocol_id id) noexcept {
  switch (id) {
    case protocol_id::deep: return "DEEP";
    case protocol_id::tops: return "TOPS";
  }
  return "<unknown protocol_id>";
}

}  // namespace dfr::inline v1::wire::iextp
#endif  // DFR_WIRE_IEXTP_CONSTANTS_HPP

// The 40-byte IEX-TP header and its decoder.
//
// Two chains start here: next_sequence() and next_stream_offset(). They are
// redundant with each other, which is what lets a receiver detect a corrupted
// length field that sequence numbers alone cannot see.

#ifndef DFR_WIRE_IEXTP_HEADER_HPP
#define DFR_WIRE_IEXTP_HEADER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/iextp/constants.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace dfr::inline v1 {
namespace wire::iextp {

// ---------------------------------------------------------------------------
// header
// ---------------------------------------------------------------------------

// Unlike MoldUDP64, a heartbeat is not signalled by a sentinel count. It is a
// packet with no messages and no payload, so both fields must be zero — and
// checking only one of them is a way to mistake a malformed packet for a
// heartbeat.
enum class packet_kind : std::uint8_t {
  data,
  heartbeat,
};

[[nodiscard]] constexpr std::string_view to_string(packet_kind kind) noexcept {
  switch (kind) {
    case packet_kind::data:      return "data";
    case packet_kind::heartbeat: return "heartbeat";
  }
  return "<unknown packet_kind>";
}

struct header {
  std::uint8_t version{kVersion};
  std::uint8_t reserved{0};
  std::uint16_t protocol{0};
  std::uint32_t channel{0};
  std::uint32_t session{0};
  std::uint16_t payload_length{0};
  std::uint16_t message_count{0};
  // Signed on the wire. Kept signed here rather than widened to unsigned,
  // because the specification says signed and silently reinterpreting a
  // negative value as a huge positive one would turn a publisher fault into a
  // plausible-looking offset.
  std::int64_t stream_offset{0};
  std::uint64_t first_sequence{0};
  // Nanoseconds since the Unix epoch, as the publisher saw it. Not a dfr clock
  // time_point: it comes from a different machine's clock and must never be
  // compared against a local one without an explicit correction.
  std::uint64_t send_time_ns{0};

  [[nodiscard]] constexpr packet_kind kind() const noexcept {
    return (message_count == 0 && payload_length == 0) ? packet_kind::heartbeat
                                                       : packet_kind::data;
  }

  // The sequence a receiver should expect next.
  //
  // Same message-not-packet semantics as MoldUDP64: a packet advances by its
  // message count. A heartbeat carries no messages and so does not advance.
  [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept {
    DFR_ASSERT(first_sequence <= UINT64_MAX - message_count,
               "first_sequence + message_count would overflow");
    return first_sequence + message_count;
  }

  // The stream offset the next packet must carry.
  //
  // The second, independent chain. A receiver that tracks both this and
  // next_sequence() can detect a corrupted length field, which sequence numbers
  // alone cannot see.
  [[nodiscard]] constexpr std::int64_t next_stream_offset() const noexcept {
    return stream_offset + static_cast<std::int64_t>(payload_length);
  }

  [[nodiscard]] friend constexpr bool operator==(const header&,
                                                 const header&) = default;
};

static_assert(std::is_trivially_copyable_v<header>);

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Read the 40-byte header.
//
// Validates the version, because a version other than 1 means every offset
// below is a guess. Does not validate the block framing; that is
// message_cursor's job.
[[nodiscard]] constexpr result<header> decode_header(packet_view packet) noexcept {
  packet_view fields;
  if (const auto err = packet.prefix(kHeaderSize).get(fields);
      err != error::ok) DFR_UNLIKELY {
    return error::truncated_header;
  }

  const std::uint8_t version = fields.u8_at(kVersionOffset);
  if (version != kVersion) DFR_UNLIKELY {
    // Deliberately not "unknown message type": the *transport* version is
    // wrong, so nothing about the rest of the packet can be trusted, including
    // the field offsets used to read it.
    return error::not_supported;
  }

  return header{
      .version = version,
      .reserved = fields.u8_at(kReservedOffset),
      .protocol = fields.le16_at(kProtocolIdOffset),
      .channel = fields.le32_at(kChannelIdOffset),
      .session = fields.le32_at(kSessionIdOffset),
      .payload_length = fields.le16_at(kPayloadLengthOffset),
      .message_count = fields.le16_at(kMessageCountOffset),
      .stream_offset =
          static_cast<std::int64_t>(fields.le64_at(kStreamOffsetOffset)),
      .first_sequence = fields.le64_at(kFirstSequenceOffset),
      .send_time_ns = fields.le64_at(kSendTimeOffset),
  };
}

}  // namespace wire::iextp
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_IEXTP_HEADER_HPP

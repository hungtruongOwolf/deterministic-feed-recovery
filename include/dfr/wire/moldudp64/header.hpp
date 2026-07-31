// The 20-byte MoldUDP64 header, and the message-versus-packet sequence
// semantics that implementations get wrong.
//
// decode_header lives here rather than in a separate decoder file because it is
// the header struct's only constructor in practice; splitting them would put a
// type and the one function that produces it in different files.

#ifndef DFR_WIRE_MOLDUDP64_HEADER_HPP
#define DFR_WIRE_MOLDUDP64_HEADER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/moldudp64/constants.hpp>
#include <dfr/wire/moldudp64/session_id.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace dfr::inline v1 {
namespace wire::moldudp64 {

// ---------------------------------------------------------------------------
// header
// ---------------------------------------------------------------------------

enum class packet_kind : std::uint8_t {
  data,
  heartbeat,
  end_of_session,
};

[[nodiscard]] constexpr std::string_view to_string(packet_kind kind) noexcept {
  switch (kind) {
    case packet_kind::data:           return "data";
    case packet_kind::heartbeat:      return "heartbeat";
    case packet_kind::end_of_session: return "end_of_session";
  }
  return "<unknown packet_kind>";
}

struct header {
  session_id session{};
  // The sequence number of the FIRST message in this packet. For a heartbeat,
  // the next sequence the publisher intends to send.
  std::uint64_t sequence{0};
  std::uint16_t message_count{0};

  [[nodiscard]] constexpr packet_kind kind() const noexcept {
    if (message_count == kEndOfSession) {
      return packet_kind::end_of_session;
    }
    if (message_count == kHeartbeat) {
      return packet_kind::heartbeat;
    }
    return packet_kind::data;
  }

  // The sequence a receiver should expect next, having consumed this packet.
  //
  // This is where the message-versus-packet distinction becomes code. A data
  // packet advances by its message count; a heartbeat carries the next expected
  // sequence directly and so advances to it, not past it.
  [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept {
    switch (kind()) {
      case packet_kind::data:
        DFR_ASSERT(sequence <= UINT64_MAX - message_count,
                   "sequence + message_count would overflow; the publisher is "
                   "at the end of a 64-bit space, which is a finding");
        return sequence + message_count;
      case packet_kind::heartbeat:
      case packet_kind::end_of_session:
        return sequence;
    }
    return sequence;
  }

  [[nodiscard]] friend constexpr bool operator==(const header&,
                                                 const header&) = default;
};

static_assert(std::is_trivially_copyable_v<header>);

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Read the 20-byte header. Does not validate the block sequence; walking the
// blocks is message_cursor's job, so that a caller which only needs the
// sequence number does not pay for it.
[[nodiscard]] constexpr result<header> decode_header(packet_view packet) noexcept {
  packet_view fields;
  if (const auto err = packet.prefix(kHeaderSize).get(fields);
      err != error::ok) DFR_UNLIKELY {
    // A datagram shorter than the header is not a block problem, so it gets its
    // own code rather than block_overruns_datagram.
    return error::truncated_header;
  }

  std::array<std::byte, kSessionSize> session_bytes{};
  for (std::size_t i = 0; i < kSessionSize; ++i) {
    session_bytes[i] = fields.data()[kSessionOffset + i];
  }

  return header{
      .session = session_id{session_bytes},
      .sequence = fields.be64_at(kSequenceOffset),
      .message_count = fields.be16_at(kMessageCountOffset),
  };
}

}  // namespace wire::moldudp64
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_MOLDUDP64_HEADER_HPP

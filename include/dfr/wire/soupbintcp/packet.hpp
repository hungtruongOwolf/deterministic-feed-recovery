// One SoupBinTCP packet, decoded from a buffer that may not hold all of it yet.
//
// The distinction this file exists to keep is between *wrong* and *not yet*. A datagram protocol has
// only the first: a UDP packet either contains a whole header or it is malformed. A byte stream has
// both, and a reader that reports a half-arrived packet as truncated will discard data that was
// merely still in flight.

#ifndef DFR_WIRE_SOUPBINTCP_PACKET_HPP
#define DFR_WIRE_SOUPBINTCP_PACKET_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/soupbintcp/constants.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::wire::soupbintcp {

struct packet {
  packet_type type{packet_type::server_heartbeat};

  // The payload, excluding the length and type bytes. Empty for a heartbeat, an end-of-session or a
  // logout, and that is the normal case rather than a degenerate one.
  packet_view payload{};

  // Total bytes this packet occupied in the stream, so a caller knows how far to advance. Includes
  // the length field, which the length field itself does not.
  std::size_t frame_size{0};

  [[nodiscard]] constexpr bool advances_sequence() const noexcept {
    return soupbintcp::advances_sequence(type);
  }
};

// Decodes one packet from the front of `stream`.
//
// Returns `need_more_bytes` when the buffer holds a prefix of a packet, and reports it without
// consuming anything, so a caller can append more bytes and call again with the same buffer. That is
// the same discipline capture::pcap::reader follows for a truncated record, and for the same reason:
// a failed read that had already advanced the cursor would leave nothing behind to retry with.
[[nodiscard]] constexpr result<packet> decode(packet_view stream) noexcept {
  if (stream.size() < kFrameOverhead) DFR_UNLIKELY {
    return error::need_more_bytes;
  }

  // Big-endian, unlike IEX-TP and like MoldUDP64. The two live in the same repository, so the
  // accessors name their order and this call site cannot be read the wrong way round.
  const std::uint16_t declared = stream.be16_at(kLengthOffset);
  if (declared < kMinPacketLength) DFR_UNLIKELY {
    // Zero would claim a packet with no type byte, which is not a packet. A real length of zero
    // cannot happen, so this is corruption rather than an empty message.
    return error::truncated_header;
  }
  if (static_cast<std::size_t>(declared) + kLengthSize > kMaxPacketBytes) DFR_UNLIKELY {
    // Refused rather than waited for: a corrupted length would otherwise stall the session
    // indefinitely on one bad packet, waiting for bytes the sender never intends to send.
    return error::capacity_exceeded;
  }

  // The length counts the type byte but not itself, so the frame is two bytes longer than it claims.
  // This is the one arithmetic in the protocol that is easy to get wrong, and it is wrong only for
  // packets that carry a payload: heartbeats work either way, which is what lets the mistake
  // survive testing.
  const std::size_t frame = kLengthSize + static_cast<std::size_t>(declared);
  if (stream.size() < frame) {
    return error::need_more_bytes;
  }

  const std::size_t payload_size = static_cast<std::size_t>(declared) - kTypeSize;
  packet_view payload{};
  if (payload_size > 0) {
    if (const auto err = stream.subview(kFrameOverhead, payload_size).get(payload);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
  }

  return packet{.type = static_cast<packet_type>(stream.u8_at(kTypeOffset)),
                .payload = payload,
                .frame_size = frame};
}

// Whether a byte is a packet type this build knows.
//
// Separate from decode(), which accepts any type byte on purpose: an unknown type is a packet whose
// *framing* is valid, and a session that dropped the connection over one would be less robust than
// the specification requires. A caller decides what to do with it.
[[nodiscard]] constexpr bool is_known(packet_type type) noexcept {
  switch (type) {
    case packet_type::debug:
    case packet_type::login_accepted:
    case packet_type::login_rejected:
    case packet_type::login_request:
    case packet_type::sequenced_data:
    case packet_type::unsequenced_data:
    case packet_type::server_heartbeat:
    case packet_type::client_heartbeat:
    case packet_type::end_of_session:
    case packet_type::logout_request:
      return true;
  }
  return false;
}

}  // namespace dfr::inline v1::wire::soupbintcp
#endif  // DFR_WIRE_SOUPBINTCP_PACKET_HPP

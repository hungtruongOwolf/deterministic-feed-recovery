// SoupBinTCP 3.00 field offsets and packet types.
//
// The session layer under both of NASDAQ's TCP protocols: Glimpse (snapshot recovery) and OUCH
// (order entry). Nine pages of specification, and two of its properties cause most of the bugs.
//
// Packet Length counts the type byte, and not itself
// -------------------------------------------------
// "The length of the packet, in bytes, not including the Packet Length field itself" — so a
// heartbeat, which has no payload, has Packet Length 1. An implementation that reads it as a
// *payload* length is off by one on every packet, and it works perfectly until it meets real data,
// because the overwhelming majority of packets on an idle session are heartbeats with no payload at
// all. That is the shape of a defect that reaches production.
//
// Sequence numbers are implicit and never on the wire
// -------------------------------------------------
// A Sequenced Data Packet carries no number. Login Accepted states the number of the *next*
// sequenced packet, and the client counts from there. If a client loses count it cannot recover it
// from the stream at all — there is nothing in the bytes to resynchronise against — so its only
// repair is to log out and log back in naming the sequence it wants.
//
// That is the exact opposite of MoldUDP64 and IEX-TP, where every datagram states its own position,
// and it is worth stating plainly because a receiver written for those and then pointed at this one
// inherits an assumption that is now false.

#ifndef DFR_WIRE_SOUPBINTCP_CONSTANTS_HPP
#define DFR_WIRE_SOUPBINTCP_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace wire::soupbintcp {

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

inline constexpr std::size_t kLengthOffset = 0;
inline constexpr std::size_t kLengthSize = 2;
inline constexpr std::size_t kTypeOffset = 2;
inline constexpr std::size_t kTypeSize = 1;

// Everything before the payload. Not the same as the value in the length field, which counts the
// type byte but not itself — the two are deliberately given different names so a reader cannot use
// one where the other belongs.
inline constexpr std::size_t kFrameOverhead = kLengthSize + kTypeSize;

// The smallest legal value of the Packet Length field: one type byte and no payload.
inline constexpr std::uint16_t kMinPacketLength = 1;

// A ceiling on what will be accepted from the wire. The specification does not state one, so this is
// ours: a two-byte length field can claim 65,535 bytes, and a reader that waited for whatever a
// corrupted length asked for would stall a session on one bad packet. Generous enough for any real
// OUCH or Glimpse payload.
inline constexpr std::size_t kMaxPacketBytes = 8'192;

// ---------------------------------------------------------------------------
// Packet types
// ---------------------------------------------------------------------------

enum class packet_type : std::uint8_t {
  // Server → client. Free text, and it may arrive at any time — including between two Sequenced
  // Data Packets, which is why it must never advance the implicit sequence.
  debug = '+',

  // Server → client. Session (10 bytes) and the sequence number of the next Sequenced Data Packet
  // (20 bytes, numeric ASCII).
  login_accepted = 'A',
  // Server → client. One reason byte.
  login_rejected = 'J',
  // Client → server. Username, password, requested session, requested sequence number.
  login_request = 'L',

  // Server → client, and the only type that advances the implicit sequence.
  sequenced_data = 'S',
  // Client → server. Carries no position: an order entry message is not part of a numbered stream.
  unsequenced_data = 'U',

  server_heartbeat = 'H',
  client_heartbeat = 'R',
  end_of_session = 'Z',
  logout_request = 'O',
};

// Whether a type advances the sequence a client is counting. Exactly one does, and writing it as a
// function rather than a comment means a caller cannot forget which.
[[nodiscard]] constexpr bool advances_sequence(packet_type type) noexcept {
  return type == packet_type::sequenced_data;
}

// Whether the server sent it. Used to catch a session wired backwards, which otherwise presents as
// a stream of packets that decode perfectly and mean nothing.
[[nodiscard]] constexpr bool from_server(packet_type type) noexcept {
  switch (type) {
    case packet_type::debug:
    case packet_type::login_accepted:
    case packet_type::login_rejected:
    case packet_type::sequenced_data:
    case packet_type::server_heartbeat:
    case packet_type::end_of_session:
      return true;
    case packet_type::login_request:
    case packet_type::unsequenced_data:
    case packet_type::client_heartbeat:
    case packet_type::logout_request:
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Login Accepted
// ---------------------------------------------------------------------------

inline constexpr std::size_t kSessionSize = 10;
inline constexpr std::size_t kSequenceTextSize = 20;
inline constexpr std::size_t kLoginAcceptedPayload = kSessionSize + kSequenceTextSize;

// ---------------------------------------------------------------------------
// Login Request
// ---------------------------------------------------------------------------

inline constexpr std::size_t kUsernameSize = 6;
inline constexpr std::size_t kPasswordSize = 10;
inline constexpr std::size_t kLoginRequestPayload =
    kUsernameSize + kPasswordSize + kSessionSize + kSequenceTextSize;

// ---------------------------------------------------------------------------
// Login Rejected
// ---------------------------------------------------------------------------

enum class reject_reason : std::uint8_t {
  not_authorized = 'A',
  session_not_available = 'S',
};

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

// The specification's numbers: a heartbeat each way every second, and a session considered dead
// after fifteen seconds of silence. Named rather than left to the caller because they are part of
// the protocol, not a tuning choice.
inline constexpr std::int64_t kHeartbeatIntervalMillis = 1'000;
inline constexpr std::int64_t kSilenceTimeoutMillis = 15'000;

}  // namespace wire::soupbintcp
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_SOUPBINTCP_CONSTANTS_HPP

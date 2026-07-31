// Glimpse: the snapshot protocol, as bytes rather than as an abstraction.
//
// What was missing
// ----------------
// `venue::snapshot_facility` already models the interesting *behaviour* — it captures its position at request
// time, so it can lose the race against a live feed, which is the defect the whole `glimpse` run exists to
// demonstrate. What it did not do is speak a protocol. A snapshot arrived as a `snapshot_reply` struct: a
// session and a sequence number, handed over in memory.
//
// That left the one question a snapshot protocol actually has to answer untested: **how does the client learn
// where the snapshot ends and the live feed begins?** In a struct, it is a field. On a wire it is a message that
// has to arrive, be recognised, and be trusted — and a client that mistook a snapshot message for a live one
// would apply state twice.
//
// How Glimpse works, and the one thing it is easy to get wrong
// -----------------------------------------------------------
// Glimpse is a SoupBinTCP session, not a datagram feed. The client logs in, and the server sends the current
// state as ordinary Sequenced Data Packets — the same message types the live feed carries, so a receiver needs
// no second decoder. The snapshot ends with an **End Of Snapshot** message carrying the sequence number the
// state is valid as of, and then the session ends.
//
// The easy mistake is treating that number as "the last message included". It is the sequence of the *next*
// message — the first one the client must take from the live feed — which is the same convention SoupBinTCP's
// own Login Accepted uses and the same one `snapshot_reply::next_sequence` uses. Off by one here means either
// replaying one message twice or dropping one forever, and both look like a working snapshot on a quiet feed.

#ifndef DFR_WIRE_GLIMPSE_MESSAGES_HPP
#define DFR_WIRE_GLIMPSE_MESSAGES_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace wire::glimpse {

// The message types Glimpse adds to whatever the feed already carries.
//
// Two, and they are deliberately outside the DEEP type table: a Glimpse stream carries DEEP messages *plus*
// these, and giving them values DEEP already uses would make a snapshot indistinguishable from a quote.
enum class message_type : std::uint8_t {
  // Sent first, so a client that connected to the wrong service finds out before applying anything.
  begin_snapshot = 'g',
  // Sent last, carrying the sequence the state is valid as of.
  end_snapshot = 'G',
};

[[nodiscard]] constexpr std::string_view name_of(message_type value) noexcept {
  switch (value) {
    case message_type::begin_snapshot: return "begin_snapshot";
    case message_type::end_snapshot:   return "end_snapshot";
  }
  DFR_UNREACHABLE("unnamed Glimpse message type");
}

[[nodiscard]] constexpr bool is_glimpse_type(std::uint8_t byte) noexcept {
  return byte == static_cast<std::uint8_t>(message_type::begin_snapshot) ||
         byte == static_cast<std::uint8_t>(message_type::end_snapshot);
}

inline constexpr std::size_t kTypeOffset = 0;
inline constexpr std::size_t kSessionOffset = 1;
inline constexpr std::size_t kSequenceOffset = 5;
inline constexpr std::size_t kMessageSize = 13;

struct begin_snapshot {
  /** Which session's state this is. A snapshot from another session is worthless, not merely stale. */
  std::uint32_t session{0};
};

struct end_snapshot {
  std::uint32_t session{0};
  /**
   * The sequence of the **next** message, not the last one included.
   *
   * The same convention as SoupBinTCP's Login Accepted and `venue::snapshot_reply`. Off by one either way looks
   * like a working snapshot on a quiet feed and corrupts a book on a busy one.
   */
  std::uint64_t next_sequence{0};
};

[[nodiscard]] constexpr result<std::size_t> encode_begin(mutable_packet_view out,
                                                         std::uint32_t session) noexcept {
  if (!out.contains(0, kMessageSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(kTypeOffset, static_cast<std::uint8_t>(message_type::begin_snapshot));
  out.put_le32_at(kSessionOffset, session);
  // Zero, because a begin has no position to report. Written rather than left alone so the two messages are the
  // same width and a reader cannot frame one as the other.
  out.put_le64_at(kSequenceOffset, 0);
  return kMessageSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_end(mutable_packet_view out,
                                                       std::uint32_t session,
                                                       std::uint64_t next_sequence) noexcept {
  if (!out.contains(0, kMessageSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(kTypeOffset, static_cast<std::uint8_t>(message_type::end_snapshot));
  out.put_le32_at(kSessionOffset, session);
  out.put_le64_at(kSequenceOffset, next_sequence);
  return kMessageSize;
}

[[nodiscard]] constexpr result<begin_snapshot> decode_begin(packet_view message) noexcept {
  if (message.size() != kMessageSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(kTypeOffset) != static_cast<std::uint8_t>(message_type::begin_snapshot))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  return begin_snapshot{.session = message.le32_at(kSessionOffset)};
}

[[nodiscard]] constexpr result<end_snapshot> decode_end(packet_view message) noexcept {
  if (message.size() != kMessageSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(kTypeOffset) != static_cast<std::uint8_t>(message_type::end_snapshot))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  const auto next = message.le64_at(kSequenceOffset);
  // A snapshot valid as of sequence zero is not a snapshot: sequence numbering starts at one, so zero means the
  // server never set the field. Refusing here is the difference between a client that reports a bad snapshot and
  // one that resumes the live feed from the beginning of the day.
  if (next == 0) DFR_UNLIKELY {
    return error::snapshot_stale;
  }
  return end_snapshot{.session = message.le32_at(kSessionOffset), .next_sequence = next};
}

}  // namespace wire::glimpse
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_GLIMPSE_MESSAGES_HPP

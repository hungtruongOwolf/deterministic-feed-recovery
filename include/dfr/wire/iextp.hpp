// IEX-TP — IEX's sequenced multicast transport.
//
// Layout, from the IEX Transport Specification. Every field is LITTLE-endian,
// which is the opposite of MoldUDP64 and the reason byte_order.hpp puts the
// order in each accessor's name rather than inferring it:
//
//   offset  size  field
//        0     1  Version                        0x01
//        1     1  Reserved
//        2     2  Message Protocol ID            identifies DEEP, TOPS, ...
//        4     4  Channel ID
//        8     4  Session ID
//       12     2  Payload Length                 bytes of message blocks
//       14     2  Message Count
//       16     8  Stream Offset                  signed byte offset of the first
//                                                message within the session
//       24     8  First Message Sequence Number
//       32     8  Send Time                      nanoseconds since Unix epoch
//       40     -  Message Blocks                 Message Count of them, each:
//                                                  2 bytes LE Message Length
//                                                  Message Length bytes
//
// What IEX-TP has that MoldUDP64 does not, and why it matters here:
//
//   Payload Length and Stream Offset are *redundant* with the block framing and
//   with the sequence numbers respectively. That redundancy is a gift to a
//   correctness oracle. A receiver can verify three independent chains across
//   consecutive packets:
//
//     first_sequence + message_count  ==  next packet's first_sequence
//     stream_offset   + payload_length ==  next packet's stream_offset
//     sum of block lengths + 2 per block == payload_length
//
//   If any two disagree, something is wrong that a single chain could not have
//   detected. MoldUDP64 offers only the first of the three, so a corrupted
//   length field there is invisible until the book goes wrong. This is why the
//   free IEX HIST corpus is the right thing to build against first.
//
// ---------------------------------------------------------------------------
// LIMIT, stated deliberately rather than discovered later
// ---------------------------------------------------------------------------
//
// The field offsets above are transcribed from the specification. They have NOT
// yet been validated against a real capture. The live URL for IEX-TP 1.25 now
// serves a one-page "this document has moved" stub, and the complete 15-page
// version is only in the Internet Archive, so the transcription has a single
// source.
//
// Until a real IEX HIST pcap has been parsed end to end with all three chains
// above holding across every packet, treat this decoder as unverified. The
// tests here are self-consistency tests: they prove the encoder and decoder
// agree with each other and with hand-assembled bytes matching the table above.
// They cannot prove the table is right.
//
// Validating against `iextrading.com/api/1.0/hist` is the next task, and the
// check that settles it is that a whole day of DEEP decodes with zero chain
// breaks — which is exactly the measurement that says the layout is correct.

#ifndef DFR_WIRE_IEXTP_HPP
#define DFR_WIRE_IEXTP_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace wire::iextp {

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

struct message {
  std::uint64_t sequence{0};
  packet_view payload;

  [[nodiscard]] friend bool operator==(const message&,
                                       const message&) = default;
};

// Walks the message blocks of one datagram.
//
// Mirrors moldudp64::message_cursor deliberately, so that the recovery layer
// can be written once against a transport concept rather than twice. The one
// substantive difference is that this cursor has Payload Length to check
// against, which gives it a framing oracle MoldUDP64 lacks.
class message_cursor {
 public:
  // Exhausted. Needed because result<T> stores the value beside the error; the
  // state is meaningful rather than a sentinel, exactly as for MoldUDP64.
  constexpr message_cursor() noexcept = default;

  [[nodiscard]] static constexpr result<message_cursor> over(
      packet_view packet) noexcept {
    header decoded;
    if (const auto err = decode_header(packet).get(decoded);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }

    packet_view after_header;
    if (const auto err = packet.suffix(kHeaderSize).get(after_header);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }

    // The header declares how many payload bytes follow. Trusting the datagram
    // length instead would accept a packet padded by a switch or truncated by a
    // capture, and silently decode the wrong number of blocks.
    packet_view payload;
    if (const auto err = after_header.prefix(decoded.payload_length).get(payload);
        err != error::ok) DFR_UNLIKELY {
      return error::block_overruns_datagram;
    }

    // Bytes beyond the declared payload are not ours to interpret. Reported
    // rather than ignored: on a real feed this means either the capture is
    // padded or the length field is wrong, and both are findings.
    if (after_header.size() != decoded.payload_length) DFR_UNLIKELY {
      return error::trailing_bytes;
    }

    return message_cursor{decoded, payload};
  }

  [[nodiscard]] constexpr const header& packet_header() const noexcept {
    return header_;
  }
  [[nodiscard]] constexpr std::uint16_t remaining() const noexcept {
    return remaining_;
  }
  [[nodiscard]] constexpr bool done() const noexcept { return remaining_ == 0; }
  [[nodiscard]] constexpr packet_view rest() const noexcept { return payload_; }

  [[nodiscard]] constexpr result<message> next() noexcept {
    DFR_ASSERT(!done(), "next() on an exhausted cursor; check done() first");

    packet_view length_field;
    if (const auto err = payload_.prefix(kMessageLengthSize).get(length_field);
        err != error::ok) DFR_UNLIKELY {
      return error::block_count_overstated;
    }
    const std::uint16_t length = length_field.le16_at(0);

    if (const auto err = payload_.consume(kMessageLengthSize); !err)
        DFR_UNLIKELY {
      return err.error_code();
    }

    packet_view body;
    if (const auto err = payload_.prefix(length).get(body); err != error::ok)
        DFR_UNLIKELY {
      return error::block_overruns_datagram;
    }
    if (const auto err = payload_.consume(length); !err) DFR_UNLIKELY {
      return err.error_code();
    }

    DFR_MAYBE(length == 0);

    const std::uint64_t sequence = next_sequence_;
    ++next_sequence_;
    --remaining_;
    return message{.sequence = sequence, .payload = body};
  }

  template <typename Handler>
  [[nodiscard]] constexpr result<void> drain(Handler&& handler) noexcept {
    while (!done()) {
      message current;
      if (const auto err = next().get(current); err != error::ok) DFR_UNLIKELY {
        return err;
      }
      handler(current);
    }
    // Blocks consumed exactly the declared payload, or the count under-states.
    if (!payload_.empty()) DFR_UNLIKELY {
      return error::trailing_bytes;
    }
    return ok();
  }

 private:
  constexpr message_cursor(header decoded, packet_view payload) noexcept
      : header_(decoded),
        payload_(payload),
        next_sequence_(decoded.first_sequence),
        remaining_(decoded.message_count) {}

  header header_;
  packet_view payload_;
  std::uint64_t next_sequence_{0};
  std::uint16_t remaining_{0};
};

// ---------------------------------------------------------------------------
// The three-chain check
// ---------------------------------------------------------------------------

// Tracks what consecutive packets on one channel must satisfy.
//
// This is the reason to build against IEX first. Two of these three chains are
// redundant with the third, and redundancy is what turns a decoder into an
// oracle: a corrupted Payload Length is invisible to a sequence-number check
// and vice versa, so a receiver that verifies both detects a class of fault that
// neither alone can see.
class chain_checker {
 public:
  constexpr chain_checker() noexcept = default;

  // Feeds one packet's header. Returns the first inconsistency found, if any.
  //
  // The first packet establishes the chain rather than being checked against
  // nothing, so a receiver that joins a live feed mid-session does not report a
  // spurious error on its first packet.
  [[nodiscard]] constexpr result<void> observe(const header& value) noexcept {
    if (!started_) {
      started_ = true;
      session_ = value.session;
      expected_sequence_ = value.next_sequence();
      expected_offset_ = value.next_stream_offset();
      return ok();
    }

    if (value.session != session_) DFR_UNLIKELY {
      // Fatal: every sequence number and stream offset held refers to the old
      // session.
      session_ = value.session;
      expected_sequence_ = value.next_sequence();
      expected_offset_ = value.next_stream_offset();
      return error::session_changed;
    }

    if (value.first_sequence != expected_sequence_) DFR_UNLIKELY {
      const bool behind = value.first_sequence < expected_sequence_;
      // Resynchronise either way, so one gap does not produce an error on every
      // subsequent packet.
      expected_sequence_ = value.next_sequence();
      expected_offset_ = value.next_stream_offset();
      return behind ? error::sequence_regressed : error::sequence_gap;
    }

    if (value.stream_offset != expected_offset_) DFR_UNLIKELY {
      // Sequence numbers chained correctly and byte offsets did not. Neither
      // check alone would have found this.
      expected_offset_ = value.next_stream_offset();
      return error::message_length_mismatch;
    }

    expected_sequence_ = value.next_sequence();
    expected_offset_ = value.next_stream_offset();
    return ok();
  }

  [[nodiscard]] constexpr bool started() const noexcept { return started_; }
  [[nodiscard]] constexpr std::uint64_t expected_sequence() const noexcept {
    return expected_sequence_;
  }
  [[nodiscard]] constexpr std::int64_t expected_stream_offset() const noexcept {
    return expected_offset_;
  }

 private:
  std::uint64_t expected_sequence_{0};
  std::int64_t expected_offset_{0};
  std::uint32_t session_{0};
  bool started_{false};
};

// The third chain, checked within one packet rather than across two: the block
// framing must account for exactly the declared payload length.
//
// Separate from chain_checker because it needs the payload bytes, not just the
// header, and because a caller that only reads headers should still be able to
// use the cross-packet chains.
[[nodiscard]] constexpr result<void> verify_payload_framing(
    packet_view packet) noexcept {
  message_cursor cursor;
  if (const auto err = message_cursor::over(packet).get(cursor);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return cursor.drain([](const message&) {});
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr result<std::size_t> encode_header(
    mutable_packet_view out, const header& value) noexcept {
  if (!out.contains(0, kHeaderSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }

  out.put_u8_at(kVersionOffset, value.version);
  out.put_u8_at(kReservedOffset, value.reserved);
  out.put_le16_at(kProtocolIdOffset, value.protocol);
  out.put_le32_at(kChannelIdOffset, value.channel);
  out.put_le32_at(kSessionIdOffset, value.session);
  out.put_le16_at(kPayloadLengthOffset, value.payload_length);
  out.put_le16_at(kMessageCountOffset, value.message_count);
  out.put_le64_at(kStreamOffsetOffset,
                  static_cast<std::uint64_t>(value.stream_offset));
  out.put_le64_at(kFirstSequenceOffset, value.first_sequence);
  out.put_le64_at(kSendTimeOffset, value.send_time_ns);
  return kHeaderSize;
}

// Builds a datagram, computing Payload Length and Message Count on finish().
//
// Same reasoning as the MoldUDP64 builder, and it matters more here: this
// encoder maintains two redundant fields, so an implementation that let a
// caller supply either one could produce a packet that fails its own chain
// check. dfr::chaos will inject exactly that; the honest encoder must be unable
// to do it by accident.
class packet_builder {
 public:
  constexpr packet_builder() noexcept = default;

  [[nodiscard]] static constexpr result<packet_builder> into(
      mutable_packet_view buffer, const header& prototype) noexcept {
    if (!buffer.contains(0, kHeaderSize)) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }
    return packet_builder{buffer, prototype};
  }

  [[nodiscard]] constexpr result<void> append(packet_view body) noexcept {
    if (body.size() > UINT16_MAX) DFR_UNLIKELY {
      return error::invalid_argument;
    }

    const std::size_t needed = kMessageLengthSize + body.size();
    if (payload_used_ + needed > UINT16_MAX) DFR_UNLIKELY {
      // Payload Length is 16 bits, so a packet cannot describe more than this
      // regardless of how large the buffer is.
      return error::capacity_exceeded;
    }
    if (!buffer_.contains(kHeaderSize + payload_used_, needed)) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    const std::size_t at = kHeaderSize + payload_used_;
    buffer_.put_le16_at(at, static_cast<std::uint16_t>(body.size()));
    for (std::size_t i = 0; i < body.size(); ++i) {
      buffer_.put_u8_at(at + kMessageLengthSize + i,
                        static_cast<std::uint8_t>(body.data()[i]));
    }
    payload_used_ += needed;
    ++count_;
    return ok();
  }

  [[nodiscard]] constexpr result<packet_view> finish() noexcept {
    header value = prototype_;
    value.payload_length = static_cast<std::uint16_t>(payload_used_);
    value.message_count = count_;

    std::size_t written = 0;
    if (const auto err = encode_header(buffer_, value).get(written);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    DFR_ASSERT(written == kHeaderSize, "encode_header wrote a short header");

    packet_view finished;
    if (const auto err =
            buffer_.as_const().prefix(kHeaderSize + payload_used_).get(finished);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    return finished;
  }

  [[nodiscard]] constexpr std::uint16_t message_count() const noexcept {
    return count_;
  }
  [[nodiscard]] constexpr std::size_t size() const noexcept {
    return kHeaderSize + payload_used_;
  }

 private:
  constexpr packet_builder(mutable_packet_view buffer,
                           const header& prototype) noexcept
      : buffer_(buffer), prototype_(prototype) {}

  mutable_packet_view buffer_;
  header prototype_{};
  std::size_t payload_used_{0};
  std::uint16_t count_{0};
};

// A heartbeat: header only, with both count and payload length zero. Checking
// only one of the two would let a malformed packet pass as a heartbeat.
[[nodiscard]] constexpr result<std::size_t> encode_heartbeat(
    mutable_packet_view out, const header& prototype) noexcept {
  header value = prototype;
  value.message_count = 0;
  value.payload_length = 0;
  return encode_header(out, value);
}

}  // namespace wire::iextp
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_IEXTP_HPP

// MoldUDP64 — NASDAQ's sequenced multicast transport.
//
// Layout, from the Nasdaq MoldUDP64 Protocol Specification V1.00:
//
//   offset  size  field
//        0    10  Session          alphanumeric, left-justified, space-padded
//       10     8  Sequence Number  big-endian, of the FIRST message in the packet
//       18     2  Message Count    big-endian
//       20     -  Message Blocks   Message Count of them, each:
//                                    2 bytes big-endian Message Length
//                                    Message Length bytes of payload
//
// The single most important semantic, and the one implementations get wrong:
//
//   **Sequence Number counts MESSAGES, not PACKETS.** A packet carrying three
//   messages at sequence 100 means messages 100, 101 and 102, and the next
//   packet must begin at 103. A client that increments by one per packet falls
//   behind by (message_count - 1) on every packet and then reports a gap that
//   does not exist — or worse, silently accepts one that does.
//
// Two special packets, both signalled through Message Count:
//
//   Message Count 0       heartbeat. Carries no messages, and its Sequence
//                         Number is the *next* sequence the publisher will
//                         send. So a heartbeat advances a watermark; treating
//                         it as a data packet at that sequence reports a
//                         spurious gap. (astra-feed-engine gets this right and
//                         most recent repositories do not.)
//   Message Count 0xFFFF  end of session. No more data will follow.
//
// This header decodes and encodes. Encoding is not an afterthought: dfr::venue
// has to produce byte-identical packets in order to test a client, and
// dfr::chaos has to rewrite sequence numbers in place. The survey found that
// ITCH decoders are saturated while encoders that behave like an exchange
// number roughly zero, so the encode side is a first-class part of the library.

#ifndef DFR_WIRE_MOLDUDP64_HPP
#define DFR_WIRE_MOLDUDP64_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace wire::moldudp64 {

// ---------------------------------------------------------------------------
// Wire constants
//
// Field offsets are named constants with accessors, rather than a packed
// struct. A packed struct over a received datagram requires either a
// reinterpret_cast at an arbitrary alignment — undefined behaviour that happens
// to work on x86 — or a memcpy that then needs per-field byte swapping anyway.
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

// ---------------------------------------------------------------------------
// session_id
// ---------------------------------------------------------------------------

// The 10-byte session identifier, compared byte-exactly.
//
// Byte-exact and not trimmed, deliberately. A session change is a fatal error —
// every sequence number the client holds refers to a different stream — so the
// comparison must not be the place where a subtle equivalence is invented. Two
// sessions that differ only in padding are two different sessions as far as this
// type is concerned, and if a publisher ever does that it is a finding, not
// something to paper over.
class session_id {
 public:
  constexpr session_id() noexcept = default;

  explicit constexpr session_id(
      std::array<std::byte, kSessionSize> bytes) noexcept
      : bytes_(bytes) {}

  // From a printable name, space-padded on the right as the spec requires.
  // Rejects an over-long name rather than truncating: silently shortening a
  // session name would make two distinct sessions compare equal.
  [[nodiscard]] static constexpr result<session_id> from_text(
      std::string_view text) noexcept {
    if (text.size() > kSessionSize) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    session_id out;
    for (std::size_t i = 0; i < kSessionSize; ++i) {
      out.bytes_[i] = i < text.size()
                          ? static_cast<std::byte>(text[i])
                          : static_cast<std::byte>(' ');
    }
    return out;
  }

  [[nodiscard]] constexpr std::array<std::byte, kSessionSize> bytes()
      const noexcept {
    return bytes_;
  }

  // The identifier with trailing spaces removed, for display only. Never used
  // for comparison — see the class comment.
  [[nodiscard]] std::string_view text() const noexcept {
    std::size_t length = kSessionSize;
    while (length > 0 && bytes_[length - 1] == static_cast<std::byte>(' ')) {
      --length;
    }
    return {reinterpret_cast<const char*>(bytes_.data()), length};
  }

  [[nodiscard]] friend constexpr bool operator==(const session_id&,
                                                 const session_id&) = default;

 private:
  // Spaces rather than zeros, so that a default-constructed id round-trips
  // through the wire as a valid (if empty) session name.
  std::array<std::byte, kSessionSize> bytes_{
      std::byte{' '}, std::byte{' '}, std::byte{' '}, std::byte{' '},
      std::byte{' '}, std::byte{' '}, std::byte{' '}, std::byte{' '},
      std::byte{' '}, std::byte{' '}};
};

static_assert(sizeof(session_id) == kSessionSize,
              "session_id must be exactly the wire field, so an array of them "
              "has no padding");
static_assert(std::is_trivially_copyable_v<session_id>);

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
  session_id session;
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

// One message, with the sequence number it occupies.
struct message {
  std::uint64_t sequence{0};
  packet_view payload;

  [[nodiscard]] friend bool operator==(const message&,
                                       const message&) = default;
};

// Walks the message blocks of one datagram.
//
// An explicit cursor rather than a callback. helix and most implementations take
// a handler, which forces control flow to invert and makes it awkward to stop
// half way — which is exactly what a fault injector and a differential test
// both need to do. TIGER_STYLE's "centralize control flow" points the same way:
// the loop belongs to the caller.
//
// The cursor never trusts the header's count. Both of the ways that count can
// lie are detected:
//
//   * more blocks claimed than bytes remain      -> block_count_overstated
//   * a block's own length runs past the end     -> block_overruns_datagram
//
// This is the pair of defects in penberg/helix (moldudp64.hh:106-115).
class message_cursor {
 public:
  // A cursor over nothing: exhausted, with no messages and no bytes left.
  //
  // Required because result<message_cursor> stores the value beside the error
  // and so needs a default-constructible T — the trade documented in
  // result.hpp. It costs nothing here because the state is genuinely
  // meaningful rather than an "invalid" sentinel: done() is true, remaining()
  // is zero, rest() is empty, and next() asserts exactly as it does on any
  // other exhausted cursor. There is no state a caller can reach through this
  // constructor that is not also reachable from a heartbeat packet.
  constexpr message_cursor() noexcept = default;

  // `packet` is the whole datagram, header included. Taking the whole thing
  // rather than a pre-sliced payload means a caller cannot accidentally pass a
  // payload sliced with the wrong offset.
  [[nodiscard]] static constexpr result<message_cursor> over(
      packet_view packet) noexcept {
    header decoded;
    if (const auto err = decode_header(packet).get(decoded);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }

    packet_view payload;
    if (const auto err = packet.suffix(kHeaderSize).get(payload);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }

    return message_cursor{decoded, payload};
  }

  [[nodiscard]] constexpr const header& packet_header() const noexcept {
    return header_;
  }

  // Blocks the header still claims are present.
  //
  // Zero for a heartbeat and for end-of-session, so the ordinary
  // `while (!done())` loop handles both without a special case.
  [[nodiscard]] constexpr std::uint16_t remaining() const noexcept {
    return remaining_;
  }

  [[nodiscard]] constexpr bool done() const noexcept {
    return remaining_ == 0;
  }

  // Bytes not yet consumed. A caller should check this is empty once done(),
  // which is what catches a header that under-states its count.
  [[nodiscard]] constexpr packet_view rest() const noexcept { return payload_; }

  [[nodiscard]] constexpr result<message> next() noexcept {
    DFR_ASSERT(!done(), "next() on an exhausted cursor; check done() first");

    packet_view length_field;
    if (const auto err = payload_.prefix(kMessageLengthSize).get(length_field);
        err != error::ok) DFR_UNLIKELY {
      // The header claimed another block and there are not even two bytes left
      // for its length. The count was a lie.
      return error::block_count_overstated;
    }
    const std::uint16_t length = length_field.be16_at(0);

    if (const auto err = payload_.consume(kMessageLengthSize); !err)
        DFR_UNLIKELY {
      return err.error_code();
    }

    packet_view body;
    if (const auto err = payload_.prefix(length).get(body); err != error::ok)
        DFR_UNLIKELY {
      // The length field itself is the lie this time.
      return error::block_overruns_datagram;
    }
    if (const auto err = payload_.consume(length); !err) DFR_UNLIKELY {
      return err.error_code();
    }

    // A zero-length block is legal on the wire, and is not the end-of-session
    // marker — that is signalled by Message Count, not by a length. Recorded
    // rather than asserted, because it is a state a correct publisher may
    // produce and a reader should not be surprised by it.
    DFR_MAYBE(length == 0);

    const std::uint64_t sequence = next_sequence_;
    ++next_sequence_;
    --remaining_;
    return message{.sequence = sequence, .payload = body};
  }

  // Consumes everything and reports the first problem, if any.
  //
  // The convenience wrapper for the common case, built on the cursor rather
  // than replacing it. Also checks for trailing bytes, which a caller driving
  // the cursor by hand can forget.
  template <typename Handler>
  [[nodiscard]] constexpr result<void> drain(Handler&& handler) noexcept {
    while (!done()) {
      message current;
      if (const auto err = next().get(current); err != error::ok) DFR_UNLIKELY {
        return err;
      }
      handler(current);
    }
    if (!payload_.empty()) DFR_UNLIKELY {
      // Every block the header accounted for was consumed and bytes remain.
      // The count under-states, which loses data just as surely as over-stating
      // reads garbage.
      return error::trailing_bytes;
    }
    return ok();
  }

 private:
  // The only way to get a populated cursor is through over(), which validates.
  constexpr message_cursor(header decoded, packet_view payload) noexcept
      : header_(decoded),
        payload_(payload),
        next_sequence_(decoded.sequence),
        // A heartbeat and an end-of-session packet carry no blocks, so the
        // cursor is born exhausted and no caller needs to special-case them.
        remaining_(decoded.kind() == packet_kind::data ? decoded.message_count
                                                       : std::uint16_t{0}) {}

  header header_;
  packet_view payload_;
  std::uint64_t next_sequence_;
  std::uint16_t remaining_;
};

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// Write a header into `out`. Returns the number of bytes written.
[[nodiscard]] constexpr result<std::size_t> encode_header(
    mutable_packet_view out, const header& value) noexcept {
  if (!out.contains(0, kHeaderSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }

  const auto session_bytes = value.session.bytes();
  for (std::size_t i = 0; i < kSessionSize; ++i) {
    out.put_u8_at(kSessionOffset + i,
                  static_cast<std::uint8_t>(session_bytes[i]));
  }
  out.put_be64_at(kSequenceOffset, value.sequence);
  out.put_be16_at(kMessageCountOffset, value.message_count);
  return kHeaderSize;
}

// Builds a datagram incrementally: header first, then messages, and the count
// is written on finish() rather than declared up front.
//
// That ordering is the point. A builder that took the count as a constructor
// argument could produce a packet whose count disagrees with its contents — the
// exact defect dfr::chaos exists to inject deliberately, so the honest builder
// must be incapable of it by accident.
class packet_builder {
 public:
  // A builder over an empty buffer. Same reasoning as message_cursor's default
  // constructor: append() and finish() both report capacity_exceeded, which is
  // the correct answer for a zero-byte buffer rather than a special case.
  constexpr packet_builder() noexcept = default;

  [[nodiscard]] static constexpr result<packet_builder> into(
      mutable_packet_view buffer, session_id session,
      std::uint64_t first_sequence) noexcept {
    if (!buffer.contains(0, kHeaderSize)) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }
    return packet_builder{buffer, session, first_sequence};
  }

  // Appends one message block. Fails without modifying anything if the buffer
  // cannot hold it.
  [[nodiscard]] constexpr result<void> append(packet_view body) noexcept {
    if (body.size() > UINT16_MAX) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    if (count_ >= kMaxMessagesPerRequest) DFR_UNLIKELY {
      // Not a hard protocol limit for a data packet, but exceeding it means the
      // packet can never be requested for retransmission as a unit, so refusing
      // is more useful than allowing it.
      return error::capacity_exceeded;
    }

    const std::size_t needed = kMessageLengthSize + body.size();
    if (!buffer_.contains(used_, needed)) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    buffer_.put_be16_at(used_, static_cast<std::uint16_t>(body.size()));
    for (std::size_t i = 0; i < body.size(); ++i) {
      buffer_.put_u8_at(used_ + kMessageLengthSize + i,
                        static_cast<std::uint8_t>(body.data()[i]));
    }
    used_ += needed;
    ++count_;
    return ok();
  }

  // Writes the header, including the count, and returns the finished datagram.
  [[nodiscard]] constexpr result<packet_view> finish() noexcept {
    const header value{.session = session_,
                       .sequence = first_sequence_,
                       .message_count = count_};
    std::size_t written = 0;
    if (const auto err = encode_header(buffer_, value).get(written);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    DFR_ASSERT(written == kHeaderSize, "encode_header wrote a short header");

    packet_view finished;
    if (const auto err = buffer_.as_const().prefix(used_).get(finished);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    return finished;
  }

  [[nodiscard]] constexpr std::uint16_t message_count() const noexcept {
    return count_;
  }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return used_; }

 private:
  constexpr packet_builder(mutable_packet_view buffer, session_id session,
                           std::uint64_t first_sequence) noexcept
      : buffer_(buffer), session_(session), first_sequence_(first_sequence) {}

  mutable_packet_view buffer_;
  session_id session_;
  std::uint64_t first_sequence_;
  std::size_t used_{kHeaderSize};
  std::uint16_t count_{0};
};

// A heartbeat: header only, count zero, sequence = the next one to be sent.
[[nodiscard]] constexpr result<std::size_t> encode_heartbeat(
    mutable_packet_view out, session_id session,
    std::uint64_t next_sequence) noexcept {
  return encode_header(out, header{.session = session,
                                   .sequence = next_sequence,
                                   .message_count = kHeartbeat});
}

[[nodiscard]] constexpr result<std::size_t> encode_end_of_session(
    mutable_packet_view out, session_id session,
    std::uint64_t next_sequence) noexcept {
  return encode_header(out, header{.session = session,
                                   .sequence = next_sequence,
                                   .message_count = kEndOfSession});
}

// ---------------------------------------------------------------------------
// Retransmission requests
//
// A request has the same 20-byte shape as a data header, sent to a different
// unicast address: Sequence Number is the first message wanted and Message
// Count is how many. The count must be clamped, because the facility rejects an
// over-large request outright rather than truncating it — so a client that asks
// for a million messages gets nothing at all and then reports a timeout it
// caused itself.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::uint16_t clamp_request_count(
    std::uint64_t wanted) noexcept {
  return wanted > kMaxMessagesPerRequest
             ? kMaxMessagesPerRequest
             : static_cast<std::uint16_t>(wanted);
}

[[nodiscard]] constexpr result<std::size_t> encode_request(
    mutable_packet_view out, session_id session, std::uint64_t from_sequence,
    std::uint64_t count) noexcept {
  if (count == 0) DFR_UNLIKELY {
    // A zero-count request is a heartbeat by another name, and asking for
    // nothing is always a caller bug rather than a protocol state.
    return error::invalid_argument;
  }
  return encode_header(out,
                       header{.session = session,
                              .sequence = from_sequence,
                              .message_count = clamp_request_count(count)});
}

}  // namespace wire::moldudp64
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_MOLDUDP64_HPP

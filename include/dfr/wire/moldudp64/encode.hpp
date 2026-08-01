// Producing MoldUDP64 datagrams, and retransmission requests.
//
// dfr::venue needs byte-identical packets to test a client with, so the encoder
// is a first-class part of the library rather than a test helper.

#ifndef DFR_WIRE_MOLDUDP64_ENCODE_HPP
#define DFR_WIRE_MOLDUDP64_ENCODE_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/moldudp64/constants.hpp>
#include <dfr/wire/moldudp64/header.hpp>
#include <dfr/wire/moldudp64/session_id.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace wire::moldudp64 {

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
// argument could produce a packet whose count disagrees with its contents: the
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
// over-large request outright rather than truncating it, so a client that asks
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

#endif  // DFR_WIRE_MOLDUDP64_ENCODE_HPP

// Producing IEX-TP datagrams.
//
// The builder computes Payload Length and Message Count on finish(), so it
// cannot emit a packet that fails its own chain check: the fault dfr::chaos will
// inject deliberately.

#ifndef DFR_WIRE_IEXTP_ENCODE_HPP
#define DFR_WIRE_IEXTP_ENCODE_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/iextp/constants.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace wire::iextp {

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

#endif  // DFR_WIRE_IEXTP_ENCODE_HPP

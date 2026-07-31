// Walking the message blocks of one IEX-TP datagram.
//
// Slices the payload to the declared Payload Length rather than to the datagram
// length, because a switch may pad a short frame and a capture may truncate one.

#ifndef DFR_WIRE_IEXTP_CURSOR_HPP
#define DFR_WIRE_IEXTP_CURSOR_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/iextp/constants.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace wire::iextp {

struct message {
  std::uint64_t sequence{0};
  packet_view payload{};

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
    packet_view payload{};
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

}  // namespace wire::iextp
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_IEXTP_CURSOR_HPP

// Building SoupBinTCP packets.
//
// A first-class part of the library rather than a test helper, for the same reason the MoldUDP64 and
// IEX-TP encoders are: dfr::venue has to *be* an exchange, and an exchange that produced packets
// through a test fixture would be tested against the fixture's idea of the protocol rather than the
// protocol.
//
// Every function here writes the length field from what it actually wrote. Nothing takes a length
// from the caller, because the length that counts the type byte and not itself is the one arithmetic
// in this protocol that is easy to get wrong, and it should be got wrong in one place at most.

#ifndef DFR_WIRE_SOUPBINTCP_ENCODE_HPP
#define DFR_WIRE_SOUPBINTCP_ENCODE_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/soupbintcp/ascii.hpp>
#include <dfr/wire/soupbintcp/constants.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::soupbintcp {

// Writes a frame with the given payload, and returns how many bytes it occupied.
//
// The payload is copied. A view would be cheaper and wrong: the caller's buffer is where the frame
// goes, so a payload pointing into it would be overlapping its own destination.
[[nodiscard]] constexpr result<std::size_t> encode_packet(
    mutable_packet_view out, packet_type type, packet_view payload) noexcept {
  const std::size_t frame = kFrameOverhead + payload.size();
  if (frame > kMaxPacketBytes) DFR_UNLIKELY {
    return error::invalid_argument;
  }
  if (!out.contains(0, frame)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }

  // The type byte counts, the length field does not count itself.
  out.put_be16_at(kLengthOffset,
                  static_cast<std::uint16_t>(kTypeSize + payload.size()));
  out.put_u8_at(kTypeOffset, static_cast<std::uint8_t>(type));
  for (std::size_t i = 0; i < payload.size(); ++i) {
    out.put_u8_at(kFrameOverhead + i, payload.u8_at(i));
  }
  return frame;
}

// A packet with no payload: both heartbeats, end of session, logout.
//
// These are most of the traffic on an idle session, and they are the packets for which a wrong
// reading of the length field still works, which is why the round-trip tests deliberately use ones
// that carry a payload.
[[nodiscard]] constexpr result<std::size_t> encode_bare(mutable_packet_view out,
                                                        packet_type type) noexcept {
  return encode_packet(out, type, packet_view{});
}

[[nodiscard]] constexpr result<std::size_t> encode_login_accepted(
    mutable_packet_view out, std::string_view session,
    std::uint64_t next_sequence) noexcept {
  const std::size_t frame = kFrameOverhead + kLoginAcceptedPayload;
  if (!out.contains(0, frame)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }

  out.put_be16_at(kLengthOffset,
                  static_cast<std::uint16_t>(kTypeSize + kLoginAcceptedPayload));
  out.put_u8_at(kTypeOffset,
                static_cast<std::uint8_t>(packet_type::login_accepted));

  mutable_packet_view session_field;
  if (const auto err = out.subview(kFrameOverhead, kSessionSize).get(session_field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (const auto err = put_text_left_justified(session_field, session); !err)
      DFR_UNLIKELY {
    return err.error_code();
  }

  mutable_packet_view sequence_field;
  if (const auto err = out.subview(kFrameOverhead + kSessionSize, kSequenceTextSize)
                           .get(sequence_field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (const auto err = put_number_right_justified(sequence_field, next_sequence);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  return frame;
}

[[nodiscard]] constexpr result<std::size_t> encode_login_rejected(
    mutable_packet_view out, reject_reason reason) noexcept {
  // std::byte rather than std::uint8_t: packet_view's byte-pointer constructor is the constexpr one,
  // because the other overloads need a reinterpret_cast that constant evaluation forbids. Using the
  // wrong one here made the whole function unusable at compile time, which the compiler reported
  // rather than letting it pass as merely slow.
  const std::byte payload = static_cast<std::byte>(reason);
  return encode_packet(out, packet_type::login_rejected,
                       packet_view{&payload, 1});
}

[[nodiscard]] constexpr result<std::size_t> encode_login_request(
    mutable_packet_view out, std::string_view username, std::string_view password,
    std::string_view requested_session,
    std::uint64_t requested_sequence) noexcept {
  const std::size_t frame = kFrameOverhead + kLoginRequestPayload;
  if (!out.contains(0, frame)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }

  out.put_be16_at(kLengthOffset,
                  static_cast<std::uint16_t>(kTypeSize + kLoginRequestPayload));
  out.put_u8_at(kTypeOffset, static_cast<std::uint8_t>(packet_type::login_request));

  std::size_t at = kFrameOverhead;
  const auto put_text = [&](std::size_t width,
                            std::string_view text) -> result<void> {
    mutable_packet_view field;
    if (const auto err = out.subview(at, width).get(field); err != error::ok)
        DFR_UNLIKELY {
      return err;
    }
    at += width;
    return put_text_left_justified(field, text);
  };

  if (const auto err = put_text(kUsernameSize, username); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = put_text(kPasswordSize, password); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = put_text(kSessionSize, requested_session); !err)
      DFR_UNLIKELY {
    return err.error_code();
  }

  mutable_packet_view sequence_field;
  if (const auto err = out.subview(at, kSequenceTextSize).get(sequence_field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (const auto err =
          put_number_right_justified(sequence_field, requested_sequence);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  return frame;
}

}  // namespace dfr::inline v1::wire::soupbintcp
#endif  // DFR_WIRE_SOUPBINTCP_ENCODE_HPP

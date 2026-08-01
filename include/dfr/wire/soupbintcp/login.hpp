// The login handshake, which is where a SoupBinTCP session gets its position.
//
// Everything about recovery over TCP starts here: Login Accepted names the sequence of the next
// Sequenced Data Packet, and a client counts from it because nothing later will tell it again. Login
// Request is how a client asks to resume: the one place in the protocol where a position is chosen
// rather than observed.

#ifndef DFR_WIRE_SOUPBINTCP_LOGIN_HPP
#define DFR_WIRE_SOUPBINTCP_LOGIN_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/soupbintcp/ascii.hpp>
#include <dfr/wire/soupbintcp/packet.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::soupbintcp {

struct login_accepted {
  std::string_view session{};
  // The sequence of the *next* Sequenced Data Packet, not the last one sent. Named for what it is,
  // because the off-by-one here silently shifts every message a client will ever number.
  std::uint64_t next_sequence{0};
};

[[nodiscard]] constexpr result<login_accepted> decode_login_accepted(
    packet_view payload) noexcept {
  if (payload.size() != kLoginAcceptedPayload) DFR_UNLIKELY {
    // Exact, not "at least". A short field would be read as a shorter number, and a long one means
    // the framing was misread: both worth reporting rather than interpreting.
    return error::message_length_mismatch;
  }

  packet_view session_field;
  if (const auto err = payload.prefix(kSessionSize).get(session_field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  packet_view sequence_field;
  if (const auto err =
          payload.subview(kSessionSize, kSequenceTextSize).get(sequence_field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }

  login_accepted out;
  if (const auto err = text_left_justified(session_field).get(out.session);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (const auto err =
          number_right_justified(sequence_field).get(out.next_sequence);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return out;
}

struct login_request {
  std::string_view username{};
  std::string_view password{};
  // All spaces in the requested session means "whatever session is currently active", which decodes
  // here as an empty string rather than a sentinel: the absence is the meaning.
  std::string_view requested_session{};

  // 0 means "start me at the next message you send", 1 means "start me at the beginning of the
  // session". Those are the specification's values, and the difference between them is the whole of
  // whether a reconnecting client replays the day or joins live.
  std::uint64_t requested_sequence{0};
};

[[nodiscard]] constexpr result<login_request> decode_login_request(
    packet_view payload) noexcept {
  if (payload.size() != kLoginRequestPayload) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }

  login_request out;
  std::size_t at = 0;
  const auto take_text = [&](std::size_t width,
                             std::string_view& into) -> result<void> {
    packet_view field;
    if (const auto err = payload.subview(at, width).get(field);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    at += width;
    return text_left_justified(field).get(into) == error::ok
               ? ok()
               : result<void>{error::invalid_argument};
  };

  if (const auto err = take_text(kUsernameSize, out.username); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = take_text(kPasswordSize, out.password); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = take_text(kSessionSize, out.requested_session); !err)
      DFR_UNLIKELY {
    return err.error_code();
  }

  packet_view sequence_field;
  if (const auto err =
          payload.subview(at, kSequenceTextSize).get(sequence_field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (const auto err =
          number_right_justified(sequence_field).get(out.requested_sequence);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return out;
}

[[nodiscard]] constexpr result<reject_reason> decode_login_rejected(
    packet_view payload) noexcept {
  if (payload.size() != 1) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  const auto reason = static_cast<reject_reason>(payload.u8_at(0));
  switch (reason) {
    case reject_reason::not_authorized:
    case reject_reason::session_not_available:
      return reason;
  }
  // An unknown reason byte is reported rather than mapped onto one of the two. A client told the
  // wrong reason retries a login it should have abandoned, or abandons one it should have retried.
  return error::not_supported;
}

}  // namespace dfr::inline v1::wire::soupbintcp
#endif  // DFR_WIRE_SOUPBINTCP_LOGIN_HPP

// Fixed-width ASCII fields, which NASDAQ's TCP protocols use where the UDP ones use integers.
//
// Two padding conventions, in the same packet
// ------------------------------------------
// Login Accepted carries a Session that is *left*-justified and padded on the right with spaces, and
// a Sequence Number that is *right*-justified and padded on the left. Both in thirty bytes, adjacent.
// An implementation that trims from one end only reads one of them correctly and produces plausible
// nonsense from the other — a session id with leading spaces compares unequal to itself, and a
// sequence number read left-justified is off by a factor of ten per pad byte.
//
// So the two are separate functions with the padding in their names, and neither has a default.
//
// The twenty-digit overflow is reachable
// -------------------------------------
// A twenty-character numeric field can express 99,999,999,999,999,999,999, and a 64-bit unsigned
// integer stops at 18,446,744,073,709,551,615 — also twenty digits. So a legal-looking field can
// overflow, and the check is not defensive padding: it is the difference between reporting a
// malformed field and reporting a sequence number that has wrapped to something small and
// believable.

#ifndef DFR_WIRE_SOUPBINTCP_ASCII_HPP
#define DFR_WIRE_SOUPBINTCP_ASCII_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace wire::soupbintcp {

inline constexpr std::uint8_t kPad = ' ';

// A left-justified text field: content, then spaces. Returns the content without them.
//
// Trailing spaces only. A field whose content genuinely begins with a space is not something this
// can distinguish from padding, and the specification does not permit one — but trimming both ends
// would silently accept the other convention's field and hide the mistake.
[[nodiscard]] constexpr result<std::string_view> text_left_justified(
    packet_view field) noexcept {
  std::size_t length = field.size();
  while (length > 0 && field.u8_at(length - 1) == kPad) {
    --length;
  }
  return std::string_view{reinterpret_cast<const char*>(field.data()), length};
}

// A right-justified numeric field: spaces, then digits.
//
// Empty after trimming is `invalid_argument` rather than zero. A field of twenty spaces is a caller
// that has not filled it in, and reading it as sequence zero — which in a Login Request means
// "start me at the next message" — would turn an unset field into a meaningful instruction.
[[nodiscard]] constexpr result<std::uint64_t> number_right_justified(
    packet_view field) noexcept {
  std::size_t at = 0;
  while (at < field.size() && field.u8_at(at) == kPad) {
    ++at;
  }
  if (at == field.size()) DFR_UNLIKELY {
    return error::invalid_argument;
  }

  std::uint64_t value = 0;
  for (; at < field.size(); ++at) {
    const std::uint8_t byte = field.u8_at(at);
    if (byte < '0' || byte > '9') DFR_UNLIKELY {
      // Includes a trailing space, which would mean the field was left-justified — the other
      // convention, and a mistake worth naming rather than tolerating.
      return error::invalid_argument;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(byte - '0');
    constexpr std::uint64_t kCeiling = UINT64_MAX / 10;
    if (value > kCeiling || (value == kCeiling && digit > UINT64_MAX % 10))
        DFR_UNLIKELY {
      return error::invalid_argument;
    }
    value = value * 10 + digit;
  }
  return value;
}

// Writes text left-justified and pads the rest with spaces.
//
// Content longer than the field is refused rather than truncated: a silently shortened session id or
// username produces a login rejection whose cause is invisible in the packet that was sent.
[[nodiscard]] constexpr result<void> put_text_left_justified(
    mutable_packet_view field, std::string_view text) noexcept {
  if (text.size() > field.size()) DFR_UNLIKELY {
    return error::invalid_argument;
  }
  for (std::size_t i = 0; i < field.size(); ++i) {
    field.put_u8_at(i, i < text.size()
                           ? static_cast<std::uint8_t>(text[i])
                           : kPad);
  }
  return ok();
}

// Writes a number right-justified and pads the left with spaces.
[[nodiscard]] constexpr result<void> put_number_right_justified(
    mutable_packet_view field, std::uint64_t value) noexcept {
  if (field.size() == 0) DFR_UNLIKELY {
    return error::invalid_argument;
  }
  std::size_t at = field.size();
  std::uint64_t rest = value;
  do {
    if (at == 0) DFR_UNLIKELY {
      // The value needs more digits than the field has. Refused for the same reason as above: a
      // truncated number is a different number, and it would look like a valid one.
      return error::invalid_argument;
    }
    --at;
    field.put_u8_at(at, static_cast<std::uint8_t>('0' + rest % 10));
    rest /= 10;
  } while (rest > 0);

  for (std::size_t i = 0; i < at; ++i) {
    field.put_u8_at(i, kPad);
  }
  return ok();
}

}  // namespace wire::soupbintcp
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_SOUPBINTCP_ASCII_HPP

// The MoldUDP64 session identifier, compared byte-exactly.

#ifndef DFR_WIRE_MOLDUDP64_SESSION_ID_HPP
#define DFR_WIRE_MOLDUDP64_SESSION_ID_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/moldudp64/constants.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace dfr::inline v1 {
namespace wire::moldudp64 {

// ---------------------------------------------------------------------------
// session_id
// ---------------------------------------------------------------------------

// The 10-byte session identifier, compared byte-exactly.
//
// Byte-exact and not trimmed, deliberately. A session change is a fatal error:
// every sequence number the client holds refers to a different stream, so the
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
  // for comparison: see the class comment.
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

}  // namespace wire::moldudp64
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_MOLDUDP64_SESSION_ID_HPP

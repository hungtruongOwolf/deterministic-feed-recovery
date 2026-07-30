// Walking a pcapng option list.
//
// Every block may carry options after its fixed body:
//
//     0   2  Option Code
//     2   2  Option Length
//     4   n  Value, padded up to a 4-byte boundary
//
// terminated by code 0 with length 0. Its own file because the format is
// independent of which block carries it, and because the padding is the trap: a
// walk that advances by the unpadded length lands one to three bytes early and
// then reads part of a value as the next option's code.

#ifndef DFR_CAPTURE_PCAPNG_OPTIONS_HPP
#define DFR_CAPTURE_PCAPNG_OPTIONS_HPP

#include <dfr/capture/pcapng/constants.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace capture::pcapng {

// One option, as found.
struct option {
  std::uint16_t code{0};
  packet_view value;
};

// Calls `handler(option)` for each option until the terminator, the end of the
// buffer, or a malformed entry.
//
// Returns false when the list ended because something did not parse, so a caller
// can distinguish "no such option" from "the option list is damaged". Neither is
// an error: most blocks carry no options at all, and a reader must not reject a
// file over a trailing byte in metadata it does not use.
//
// `little` selects the byte order, which the section header established.
template <typename Handler>
[[nodiscard]] constexpr bool for_each_option(packet_view options, bool little,
                                            Handler&& handler) noexcept {
  packet_view cursor = options;

  while (cursor.size() >= kOptionHeaderSize) {
    const std::uint16_t code =
        little ? cursor.le16_at(0) : cursor.be16_at(0);
    const std::uint16_t length =
        little ? cursor.le16_at(2) : cursor.be16_at(2);

    if (code == kOptionEndOfOpt) {
      return true;
    }

    packet_view value;
    if (cursor.subview(kOptionHeaderSize, length).get(value) != error::ok) {
      return false;
    }

    if (handler(option{.code = code, .value = value})) {
      return true;  // the handler found what it wanted and asked to stop
    }

    // Padded to a four-byte boundary. Advancing by the unpadded length is the
    // defect this comment exists to prevent.
    if (!cursor.consume(kOptionHeaderSize + pad4(length))) {
      return false;
    }
  }

  // Ran out of bytes without a terminator. Common enough in real files that it is
  // not worth reporting as damage.
  return true;
}

// The `if_tsresol` value from an Interface Description Block's options, if
// present. Absent means microseconds, which is tick_resolution's default, so the
// caller keeps whatever it had.
[[nodiscard]] constexpr bool find_timestamp_resolution(
    packet_view options, bool little, std::uint8_t& raw) noexcept {
  bool found = false;
  // The walk's own return value says whether the list parsed; here it does not
  // matter, because a damaged option list simply means the interface keeps its
  // default resolution. Discarded explicitly rather than by omission, since the
  // function is [[nodiscard]] for callers that do care.
  static_cast<void>(for_each_option(options, little, [&](const option& opt) {
    if (opt.code == kOptionIfTsResolution && !opt.value.empty()) {
      raw = opt.value.u8_at(0);
      found = true;
      return true;
    }
    return false;
  }));
  return found;
}

}  // namespace capture::pcapng
}  // namespace dfr::inline v1

#endif  // DFR_CAPTURE_PCAPNG_OPTIONS_HPP

// One captured link-layer frame.
//
// The record both capture-file readers produce and the link-layer demultiplexer
// consumes, so it lives on its own rather than in either of them.

#ifndef DFR_CAPTURE_FRAME_HPP
#define DFR_CAPTURE_FRAME_HPP

#include <dfr/core/packet_view.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace capture {

// A link type, as capture files spell it. Only the one we need is named;
// anything else is reported rather than guessed at.
//
// The numbers are libpcap's LINKTYPE_* values, which pcapng reuses.
enum class link_type : std::uint16_t {
  ethernet = 1,
};

struct frame {
  // Nanoseconds since the Unix epoch, as the capturing host's clock saw it.
  //
  // Deliberately a bare integer rather than a dfr clock time_point. It comes
  // from a foreign clock: a different machine, with its own offset and drift,
  // and the type system should refuse to compare it against a local one without
  // an explicit correction. The same reasoning as IEX-TP's send_time.
  std::uint64_t timestamp_ns{0};

  // The bytes actually stored in the file.
  packet_view data{};

  // The length the frame had on the wire, which exceeds data.size() when the
  // capture was taken with a snaplen shorter than the frame.
  std::uint32_t wire_length{0};

  // Whether the capture stored less than the wire carried.
  //
  // Worth its own accessor because a silently truncated frame is how a decoder
  // comes to report a malformed packet that was fine in reality. A reader that
  // ignores this attributes a capture artefact to the publisher.
  [[nodiscard]] constexpr bool truncated() const noexcept {
    return wire_length > data.size();
  }

  [[nodiscard]] friend bool operator==(const frame&, const frame&) = default;
};

}  // namespace capture
}  // namespace dfr::inline v1

#endif  // DFR_CAPTURE_FRAME_HPP

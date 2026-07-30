// What a requester tells its caller to do, and the tally of what it has done.
//
// Split from requester.hpp for the same reason observation.hpp is split from
// gap_tracker.hpp: a caller that formats a decision, logs it or asserts on it in a
// test needs this vocabulary and never needs the state machine. One header, one
// concept, per docs/STYLE.md §1.10.

#ifndef DFR_RECOVERY_REQUEST_DECISION_HPP
#define DFR_RECOVERY_REQUEST_DECISION_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/recovery/gap.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace recovery {

// What the caller should do next.
enum class action : std::uint8_t {
  // Nothing is due. The caller waits, reads more packets, and polls again.
  idle,
  // Send a request for `range`. The requester has already recorded the attempt, so a
  // caller that fails to send must say so rather than silently dropping it.
  send,
  // Retransmission will not repair this range: the attempts are exhausted or the
  // publisher's retention window has passed. The range is dropped from the requester
  // and the caller must decide whether to take a snapshot or accept the loss.
  abandon,

  count_
};

[[nodiscard]] constexpr std::string_view name_of(action value) noexcept {
  switch (value) {
    case action::idle:    return "idle";
    case action::send:    return "send";
    case action::abandon: return "abandon";
    case action::count_:  break;
  }
  DFR_UNREACHABLE("unnamed action");
}

struct decision {
  action what{action::idle};

  // The range to request, or the range being abandoned. Empty when idle.
  sequence_range range{};

  // Which attempt this is, counting from one. Useful to a caller that logs, and the
  // number a test asserts on to prove backoff happened.
  std::uint32_t attempt{0};

  // Why the range was abandoned; `ok` for the other actions. Distinguishes "we asked
  // enough times" (retransmit_timed_out) from "it is too late to ask at all"
  // (retransmit_window_exceeded), which are different operational problems even though
  // the caller's next step is the same.
  error reason{error::ok};
};

struct requester_stats {
  std::uint64_t requests_sent{0};
  std::uint64_t ranges_abandoned{0};
  std::uint64_t messages_abandoned{0};
  std::uint64_t timed_out{0};
  std::uint64_t window_exceeded{0};

  [[nodiscard]] friend constexpr bool operator==(const requester_stats&,
                                                 const requester_stats&) = default;
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_REQUEST_DECISION_HPP

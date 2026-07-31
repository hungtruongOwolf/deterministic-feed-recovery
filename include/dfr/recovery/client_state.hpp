// What a recovery client is doing, what a packet did to it, and what the caller owes it.
//
// Split from client.hpp on the same seam as observation.hpp and request_decision.hpp: a
// caller that logs a client's state, formats a report or asserts on one in a test needs
// this vocabulary and never needs the state machine.

#ifndef DFR_RECOVERY_CLIENT_STATE_HPP
#define DFR_RECOVERY_CLIENT_STATE_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/recovery/arbiter.hpp>
#include <dfr/recovery/gap.hpp>
#include <dfr/recovery/line.hpp>
#include <dfr/recovery/observation.hpp>
#include <dfr/recovery/retransmit_policy.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace recovery {

enum class client_state : std::uint8_t {
  // No usable position in the stream yet. The first packet of a session establishes one.
  synchronising,
  // Streaming. There may be outstanding gaps being chased by retransmission; a gap is
  // not a reason to stop delivering, only a reason to ask.
  live,
  // A snapshot has been asked for and live data is being buffered rather than delivered,
  // because until the snapshot's position is known there is no way to tell which of these
  // messages it already accounts for.
  recovering,
  // The snapshot has been applied and the buffered messages that sit on top of it are
  // waiting to be handed downstream. A distinct state because the caller owes the client
  // something here: until it has replayed them and said so, offering more packets would
  // deliver messages out of order.
  replaying,
  // Recovery itself failed. There is no honest way forward from here, which is the whole
  // point of having the state: the alternative is a client that keeps producing a book it
  // knows to be wrong.
  failed,

  count_
};

inline constexpr auto kClientStateCount =
    static_cast<std::size_t>(client_state::count_);

[[nodiscard]] constexpr std::string_view name_of(client_state value) noexcept {
  switch (value) {
    case client_state::synchronising: return "synchronising";
    case client_state::live:          return "live";
    case client_state::recovering:    return "recovering";
    case client_state::replaying:     return "replaying";
    case client_state::failed:        return "failed";
    case client_state::count_:        break;
  }
  DFR_UNREACHABLE("unnamed client state");
}

// What the caller must do next. Every one of these is I/O or a decision the library
// deliberately does not own.
enum class client_action : std::uint8_t {
  idle,
  // Ask the retransmit server for `range`.
  send_retransmit_request,
  // Ask the snapshot facility for the current state of the book. Reached when
  // retransmission has given up on a range, which is the only honest response: carrying
  // on past unrecoverable loss means producing a book known to be wrong.
  request_snapshot,
  // Tear this client down and resubscribe. Recovery cannot continue.
  restart,

  count_
};

[[nodiscard]] constexpr std::string_view name_of(client_action value) noexcept {
  switch (value) {
    case client_action::idle:                    return "idle";
    case client_action::send_retransmit_request: return "send_retransmit_request";
    case client_action::request_snapshot:        return "request_snapshot";
    case client_action::restart:                 return "restart";
    case client_action::count_:                  break;
  }
  DFR_UNREACHABLE("unnamed client action");
}

struct client_decision {
  client_action what{client_action::idle};
  sequence_range range{};
  std::uint32_t attempt{0};
  error reason{error::ok};
};

// What one packet did, assembled from every layer it passed through.
//
// Returned whole rather than as several calls, because the layers disagree usefully: a
// packet can be a duplicate to the arbiter and therefore never reach the tracker at all,
// and a caller that had to ask each layer separately would have to know that.
struct ingest_report {
  // What the arbiter made of it.
  arbitration merge{arbitration::duplicate};

  // The messages that crossed the boundary for the first time. Empty for a duplicate.
  sequence_range accepted{};

  // What the sequencing looked like, for the part that was accepted. Only meaningful when
  // `accepted` is non-empty.
  sequencing outcome{sequencing::in_order};

  // The hole this packet revealed, if any. This is what a retransmit request is built
  // from, and the requester has already been told about it.
  sequence_range gap_opened{};

  // How many messages this packet filled in holes that were already known. The exact
  // sub-ranges are available from client::last_recovered(), which keeps them by reference
  // rather than copying a set into every report.
  std::uint64_t recovered{0};

  // True when the accepted messages were held for replay instead of delivered, which is
  // what `recovering` means. The caller must hand them to buffer_message().
  bool held_for_replay{false};

  // Whether anything in this packet should go downstream: either messages beyond the
  // merged stream's watermark, or messages that repaired a known hole. A retransmit has
  // the second without the first, so testing `accepted` alone would silently discard
  // every repair.
  [[nodiscard]] constexpr bool delivered() const noexcept {
    return !held_for_replay && (!accepted.empty() || recovered > 0);
  }
};

struct client_options {
  arbiter_options arbitration{};
  retransmit_policy retransmission{};

  // How many lines this client is wired to, for the liveness summary. Not enforced on
  // offer(): a packet from a line outside this count is a wiring mistake and is caught by
  // the bounds assertion instead of being silently tolerated.
  std::size_t lines{2};

  [[nodiscard]] constexpr result<void> validate() const noexcept {
    if (lines == 0 || lines > kMaxLines) {
      return error::invalid_argument;
    }
    if (const auto err = arbitration.validate(); !err) {
      return err;
    }
    return retransmission.validate();
  }
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_CLIENT_STATE_HPP

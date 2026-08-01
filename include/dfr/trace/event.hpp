// The vocabulary of a recorded run.
//
// One flat POD per event, and every event carries the *resulting* headline numbers: the client
// state, the delivered watermark, how much is missing. That redundancy is deliberate and it is the
// most important decision in this file: a viewer must be able to draw any moment in the run by
// reading one line, without replaying the stream itself.
//
// The alternative is a viewer that reconstructs state from the event sequence, which means a
// second implementation of the state machine written in a different language by someone reading
// the first one. When those two disagree, the picture is wrong and nothing says so. A trace that
// carries its own conclusions cannot drift from them.
//
// Nothing here reads a clock or allocates. `time_ns` comes from the caller's clock, which in every
// current driver is a manual_clock, so a trace is a deterministic function of the seed and is
// therefore diffable and committable next to the test that produced it.

#ifndef DFR_TRACE_EVENT_HPP
#define DFR_TRACE_EVENT_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/recovery/gap.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace trace {

using recovery::sequence_range;

// Grouped by the layer that produced it, because that is how a reader looks for things: "what did
// the venue do", "what did chaos do to it", "what did the client make of it".
enum class event_kind : std::uint8_t {
  // ---- the venue ---------------------------------------------------------
  published,
  heartbeat_sent,
  retransmit_served,
  retransmit_refused,
  snapshot_requested,
  snapshot_replied,

  // ---- chaos -------------------------------------------------------------
  fault_applied,
  packet_dropped,
  packet_duplicated,
  packet_delayed,

  // ---- the wire ----------------------------------------------------------
  // Failed to decode or to frame, so a real receiver would discard it too.
  packet_discarded,

  // ---- the client --------------------------------------------------------
  packet_accepted,
  packet_duplicate,
  gap_opened,
  gap_filled,
  session_reset,
  retransmit_requested,
  range_abandoned,
  snapshot_applied,
  snapshot_rejected,
  replay_started,
  replay_finished,
  state_changed,

  count_
};

inline constexpr auto kEventKindCount = static_cast<std::size_t>(event_kind::count_);

[[nodiscard]] constexpr std::string_view name_of(event_kind value) noexcept {
  switch (value) {
    case event_kind::published:            return "published";
    case event_kind::heartbeat_sent:       return "heartbeat_sent";
    case event_kind::retransmit_served:    return "retransmit_served";
    case event_kind::retransmit_refused:   return "retransmit_refused";
    case event_kind::snapshot_requested:   return "snapshot_requested";
    case event_kind::snapshot_replied:     return "snapshot_replied";
    case event_kind::fault_applied:        return "fault_applied";
    case event_kind::packet_dropped:       return "packet_dropped";
    case event_kind::packet_duplicated:    return "packet_duplicated";
    case event_kind::packet_delayed:       return "packet_delayed";
    case event_kind::packet_discarded:     return "packet_discarded";
    case event_kind::packet_accepted:      return "packet_accepted";
    case event_kind::packet_duplicate:     return "packet_duplicate";
    case event_kind::gap_opened:           return "gap_opened";
    case event_kind::gap_filled:           return "gap_filled";
    case event_kind::session_reset:        return "session_reset";
    case event_kind::retransmit_requested: return "retransmit_requested";
    case event_kind::range_abandoned:      return "range_abandoned";
    case event_kind::snapshot_applied:     return "snapshot_applied";
    case event_kind::snapshot_rejected:    return "snapshot_rejected";
    case event_kind::replay_started:       return "replay_started";
    case event_kind::replay_finished:      return "replay_finished";
    case event_kind::state_changed:        return "state_changed";
    case event_kind::count_:               break;
  }
  DFR_UNREACHABLE("unnamed event kind");
}

// Which layer an event came from, so a viewer can lane events without a lookup table of its own.
enum class layer : std::uint8_t { venue, chaos, wire, client, count_ };

[[nodiscard]] constexpr std::string_view name_of(layer value) noexcept {
  switch (value) {
    case layer::venue:  return "venue";
    case layer::chaos:  return "chaos";
    case layer::wire:   return "wire";
    case layer::client: return "client";
    case layer::count_: break;
  }
  DFR_UNREACHABLE("unnamed layer");
}

[[nodiscard]] constexpr layer layer_of(event_kind value) noexcept {
  switch (value) {
    case event_kind::published:
    case event_kind::heartbeat_sent:
    case event_kind::retransmit_served:
    case event_kind::retransmit_refused:
    case event_kind::snapshot_requested:
    case event_kind::snapshot_replied:
      return layer::venue;

    case event_kind::fault_applied:
    case event_kind::packet_dropped:
    case event_kind::packet_duplicated:
    case event_kind::packet_delayed:
      return layer::chaos;

    case event_kind::packet_discarded:
      return layer::wire;

    case event_kind::packet_accepted:
    case event_kind::packet_duplicate:
    case event_kind::gap_opened:
    case event_kind::gap_filled:
    case event_kind::session_reset:
    case event_kind::retransmit_requested:
    case event_kind::range_abandoned:
    case event_kind::snapshot_applied:
    case event_kind::snapshot_rejected:
    case event_kind::replay_started:
    case event_kind::replay_finished:
    case event_kind::state_changed:
      return layer::client;

    case event_kind::count_:
      break;
  }
  DFR_UNREACHABLE("event kind belongs to no layer");
}

struct event {
  // Which source packet the run had reached. The scrubber's x-axis, and deliberately not a
  // timestamp: an index is what a seed reproduces exactly, while a duration is what a machine
  // happens to take.
  std::uint64_t packet_index{0};

  // From the caller's clock. Present because backoff and liveness are about durations, and a
  // viewer showing retry intervals needs them; not used for ordering.
  std::int64_t time_ns{0};

  event_kind kind{event_kind::published};

  // Which redundant line, where the event has one.
  std::uint8_t line{0};

  // Why, for the events that have a reason. `ok` otherwise.
  error reason{error::ok};

  // The sequence range the event is about: a hole, a request, a delivery, a snapshot position.
  // Half-open, like recovery::sequence_range, so the two never need converting.
  std::uint64_t first_sequence{0};
  std::uint64_t end_sequence{0};

  // Which attempt, for retransmit requests. Zero elsewhere.
  std::uint32_t attempt{0};

  // Kind-specific: the fault_op for a chaos event, the staleness for a snapshot reply.
  std::uint64_t detail{0};

  // ---- the resulting state, so a viewer needs no domain logic -------------
  std::uint8_t client_state{0};
  std::uint64_t delivered_before{0};
  std::uint64_t messages_missing{0};
  std::uint64_t outstanding_ranges{0};

  // The outstanding holes themselves, not just how many there are.
  //
  // Added because a viewer wanted to *draw* them, and the alternative was for it to accumulate them from
  // the gap_opened and gap_filled events, which is reconstructing recovery state in another language, the
  // one thing the trace format exists to prevent. So the format grew instead, which is the rule working
  // rather than an exception to it.
  //
  // Bounded at four, because a picture with more than four holes in it stops being a picture. When there
  // are more, `outstanding_ranges` still says so and a viewer can say "and N more" rather than draw a lie.
  static constexpr std::size_t kDrawableGaps = 4;
  std::array<sequence_range, kDrawableGaps> gaps{};
  std::uint8_t gaps_drawn{0};

  // ---- the book, so a viewer can draw one without owning one --------------
  //
  // The second time the format has grown for the viewer, and the rule working rather than an exception to it. The
  // strongest thing this project asserts is that the book after loss and repair equals the book that lost nothing,
  // and until these fields existed that claim lived only in a test file: a visitor could not see it.
  //
  // A viewer that applied price levels itself would be a second implementation of an order book, written in
  // TypeScript by somebody reading the first, and when the two disagreed the drawing would be wrong with nothing to
  // say so. So the C++ keeps the book and writes down the top of it.
  //
  // Top of book only. Full depth would be up to sixteen levels a side on every one of several hundred events, which
  // is a trace an order of magnitude larger for depth a drawing this size cannot show. `book_levels` says how many
  // levels exist so a viewer can render "and N more" rather than imply the book is two levels deep.
  std::int64_t best_bid{0};
  std::uint32_t best_bid_size{0};
  std::int64_t best_ask{0};
  std::uint32_t best_ask_size{0};
  std::uint16_t book_bid_levels{0};
  std::uint16_t book_ask_levels{0};
  // Cumulative, so a viewer can show volume growing without differencing two events.
  std::uint64_t traded_shares{0};

  [[nodiscard]] friend constexpr bool operator==(const event&, const event&) = default;
};

}  // namespace trace
}  // namespace dfr::inline v1

#endif  // DFR_TRACE_EVENT_HPP

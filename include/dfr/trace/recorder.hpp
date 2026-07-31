// Recording a run, in a fixed amount of memory, and stopping rather than forgetting.
//
// When the recorder fills it stops and counts what it could not keep. It does *not* overwrite the
// oldest events — the same decision as recovery::replay_buffer and for the same reason, applied to
// diagnostics instead of data: the beginning of a trace is where the cause is. A ring would keep
// the consequences and discard the explanation, which is the shape of every unhelpful log file.
//
// The recorder is not wired into the library
// -----------------------------------------
// No component takes a recorder, and none reports into one. Every event below can be built from
// what the components already return — ingest_report, client_decision, snapshot_plan, the stats
// structs — so the caller records, exactly as it already acts. That keeps the hot path free of a
// branch it does not need, keeps the library free of a diagnostics dependency, and means a trace
// can never disagree with the values a caller saw, because it is made of them.

#ifndef DFR_TRACE_RECORDER_HPP
#define DFR_TRACE_RECORDER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/trace/event.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dfr::inline v1 {
namespace trace {

template <std::size_t Capacity = 4096>
class recorder {
 public:
  static_assert(Capacity > 0, "a recorder with no capacity would record nothing");

  constexpr recorder() noexcept = default;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] constexpr bool full() const noexcept { return count_ >= Capacity; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // How many events did not fit. Non-zero means the trace is a prefix of the run, and a viewer
  // must say so rather than presenting a truncated picture as a complete one.
  [[nodiscard]] constexpr std::uint64_t dropped() const noexcept { return dropped_; }

  [[nodiscard]] constexpr std::span<const event> events() const noexcept
      DFR_LIFETIME_BOUND {
    return {events_.data(), count_};
  }

  [[nodiscard]] constexpr const event& at(std::size_t index) const noexcept {
    DFR_ASSERT(index < count_, "reading past the recorded events");
    return events_[index];
  }

  // Records one event, or counts it as lost.
  //
  // Returns whether it was kept, so a caller that cares can stop early rather than run a long
  // simulation whose tail it will not be able to see.
  [[nodiscard]] constexpr bool record(const event& value) noexcept {
    if (count_ >= Capacity) DFR_UNLIKELY {
      ++dropped_;
      return false;
    }
    events_[count_] = value;
    ++count_;
    return true;
  }

  // How many of each kind were recorded, for a summary line that does not require the reader to
  // count. Indexed by event_kind, so adding a kind widens the table automatically.
  [[nodiscard]] constexpr std::array<std::uint64_t, kEventKindCount> by_kind()
      const noexcept {
    std::array<std::uint64_t, kEventKindCount> counts{};
    for (std::size_t i = 0; i < count_; ++i) {
      ++counts[static_cast<std::size_t>(events_[i].kind)];
    }
    return counts;
  }

  constexpr void clear() noexcept {
    count_ = 0;
    dropped_ = 0;
  }

 private:
  std::array<event, Capacity> events_{};
  std::size_t count_{0};
  std::uint64_t dropped_{0};
};

// The shape a caller fills in for every event, so the fields that describe *the run* are set in
// one place and the fields that describe *this event* at the call site.
//
// Exists because the resulting-state fields are the same for every event recorded at the same
// moment, and repeating them at twenty call sites is how one of them ends up stale — which would
// make the viewer draw a moment that never existed.
struct context {
  std::uint64_t packet_index{0};
  std::int64_t time_ns{0};
  std::uint8_t client_state{0};
  std::uint64_t delivered_through{0};
  std::uint64_t messages_missing{0};
  std::uint64_t outstanding_ranges{0};

  [[nodiscard]] constexpr event with(event_kind kind) const noexcept {
    return event{.packet_index = packet_index,
                 .time_ns = time_ns,
                 .kind = kind,
                 .client_state = client_state,
                 .delivered_through = delivered_through,
                 .messages_missing = messages_missing,
                 .outstanding_ranges = outstanding_ranges};
  }
};

}  // namespace trace
}  // namespace dfr::inline v1

#endif  // DFR_TRACE_RECORDER_HPP

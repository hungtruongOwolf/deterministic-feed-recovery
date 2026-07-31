// A snapshot service, modelled on Glimpse, that can be made to lose the race on purpose.
//
// Why the position is captured on *request* and not on *reply*
// ----------------------------------------------------------
// This is the whole reason the facility is worth building. Glimpse builds the current book and
// then transmits it, so the state a client receives reflects the feed as it was when the request
// arrived — not as it is when the last byte lands. Everything that happened in between is the
// client's problem, and the client is supposed to have been buffering it.
//
// A facility that captured its position at reply time would always hand back something at least
// as new as the client's buffer, so the race could never happen and recovery::plan_snapshot's
// behind_buffer verdict would stay a unit-tested branch that no whole client had ever been
// driven through. Capturing at request time is both more faithful and the only version that can
// fail.
//
// `staleness_messages` on top of that models the worse case: a facility served from a lagging
// replica, answering with state older than even the moment the request arrived. That is the
// configuration that produces the Glimpse race reliably rather than occasionally.
//
// One request at a time
// --------------------
// Glimpse runs over SoupBinTCP, one session, one snapshot. A second request while one is
// outstanding is refused rather than queued, because a client that fired two and applied
// whichever returned first would be choosing its own state at random.

#ifndef DFR_VENUE_SNAPSHOT_FACILITY_HPP
#define DFR_VENUE_SNAPSHOT_FACILITY_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace venue {

struct snapshot_options {
  std::uint32_t session{1};

  // How long the facility takes to build and transmit a snapshot. The window in which the race
  // can be lost is exactly this long, which is why it is a parameter and not a constant.
  duration latency{std::chrono::milliseconds{50}};

  // How far behind the request-time position the snapshot's state actually is, in messages.
  //
  // Zero is the faithful case: state as of the moment the request arrived. Above zero models a
  // facility served from a lagging replica, and it is the knob that turns the Glimpse race from
  // something that happens under load into something a test can produce on demand.
  std::uint64_t staleness_messages{0};

  [[nodiscard]] constexpr result<void> validate() const noexcept {
    if (latency < duration::zero()) {
      return error::invalid_argument;
    }
    return ok();
  }
};

// What the facility hands back. `next_sequence` is what recovery::client::on_snapshot() wants:
// the sequence of the first message *after* the snapshot's state.
struct snapshot_reply {
  std::uint32_t session{0};
  std::uint64_t next_sequence{0};
};

struct snapshot_stats {
  std::uint64_t requests{0};
  std::uint64_t replies{0};
  std::uint64_t refused_in_flight{0};
  std::uint64_t refused_before_feed{0};

  [[nodiscard]] friend constexpr bool operator==(const snapshot_stats&,
                                                 const snapshot_stats&) = default;
};

template <clock_source Clock>
class snapshot_facility {
 public:
  using time_point = typename Clock::time_point;

  explicit constexpr snapshot_facility(snapshot_options options) noexcept
      : options_(options) {
    DFR_ASSERT(options_.validate().has_value(),
               "snapshot options must be validated before use");
  }

  [[nodiscard]] constexpr const snapshot_stats& stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] constexpr bool in_flight() const noexcept { return in_flight_; }
  [[nodiscard]] constexpr std::uint64_t feed_position() const noexcept {
    return feed_position_;
  }

  // Tells the facility where the feed has reached. Driven from the publisher, so the facility
  // knows what a snapshot taken now would contain.
  constexpr void advance_to(std::uint64_t next_sequence) noexcept {
    if (next_sequence > feed_position_) {
      feed_position_ = next_sequence;
    }
  }

  // Starts building a snapshot.
  //
  // The position is captured here — see the note at the top of this file. That single line is
  // the difference between a facility that can lose the race and one that cannot.
  [[nodiscard]] constexpr result<void> request(time_point now) noexcept {
    ++stats_.requests;
    if (in_flight_) DFR_UNLIKELY {
      ++stats_.refused_in_flight;
      return error::not_supported;
    }
    if (feed_position_ == 0) DFR_UNLIKELY {
      // Nothing has been published, so there is no state to snapshot. Distinct from a stale
      // reply: the client is early rather than unlucky.
      ++stats_.refused_before_feed;
      return error::not_supported;
    }

    in_flight_ = true;
    ready_at_ = now + options_.latency;
    // Saturating rather than wrapping: a staleness larger than the feed's progress means the
    // snapshot reflects the beginning of the session, not a sequence near 2^64.
    captured_ = options_.staleness_messages >= feed_position_
                    ? 1
                    : feed_position_ - options_.staleness_messages;
    return ok();
  }

  // The reply, once the latency has elapsed. `not_supported` while nothing is outstanding, so a
  // caller polling an idle facility gets a clear answer rather than a plausible empty one.
  [[nodiscard]] constexpr result<snapshot_reply> poll(time_point now) noexcept {
    if (!in_flight_) {
      return error::not_supported;
    }
    if (now < ready_at_) {
      return error::retransmit_timed_out;  // not yet: keep polling
    }

    in_flight_ = false;
    ++stats_.replies;
    return snapshot_reply{.session = options_.session,
                          .next_sequence = captured_};
  }

  // Whether a reply is available, for a caller that would rather ask than interpret an error.
  [[nodiscard]] constexpr bool ready(time_point now) const noexcept {
    return in_flight_ && now >= ready_at_;
  }

 private:
  snapshot_options options_{};
  bool in_flight_{false};
  time_point ready_at_{};
  std::uint64_t captured_{0};
  std::uint64_t feed_position_{0};
  snapshot_stats stats_{};
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_SNAPSHOT_FACILITY_HPP

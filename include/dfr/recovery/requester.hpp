// Deciding what to ask the retransmit server for, and when to stop asking.
//
// Poll-driven, and templated on a clock it does not own
// ----------------------------------------------------
// The requester never reads a clock. It is a template on Clock only so that a
// manual_clock time point cannot be passed where a real_clock one belongs; every time
// value arrives as an argument to on_gap() or poll(). That is the structural version
// of TIGER_STYLE's *"don't do things directly in reaction to external events: your
// program should run at its own pace"*: a component that cannot read the time cannot
// be made non-deterministic by the time it is run at.
//
// It also sends nothing. poll() returns a decision and the caller performs the I/O,
// the same division itchcpp gets right and for the same reason: the moment a recovery
// library owns a socket, it owns a thread, a timer and a test harness that needs a
// network.
//
// A large hole becomes several requests up front
// ----------------------------------------------
// MoldUDP64 caps a request at 60,000 messages, so a hole wider than that is chunked
// when it is discovered rather than clamped when it is sent. Clamping at send time
// looks equivalent and is not: the pending range would still describe the whole hole,
// so every attempt would re-request the same first 60,000 messages and the rest would
// never be asked for at all.

#ifndef DFR_RECOVERY_REQUESTER_HPP
#define DFR_RECOVERY_REQUESTER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/gap.hpp>
#include <dfr/recovery/gap_set.hpp>
#include <dfr/recovery/request_decision.hpp>
#include <dfr/recovery/retransmit_policy.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace recovery {

template <clock_source Clock>
class requester {
 public:
  using time_point = typename Clock::time_point;

  explicit constexpr requester(retransmit_policy policy) noexcept
      : policy_(policy) {
    DFR_ASSERT(policy_.validate().has_value(),
               "the policy must be validated before it is used");
  }

  [[nodiscard]] constexpr const retransmit_policy& policy() const noexcept {
    return policy_;
  }
  [[nodiscard]] constexpr const requester_stats& stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] constexpr std::size_t pending() const noexcept { return count_; }

  [[nodiscard]] constexpr std::uint64_t messages_outstanding() const noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      total += entries_[i].range.count();
    }
    return total;
  }

  // Registers a hole to be recovered. Sends nothing: the first attempt is not due
  // until settle_delay has passed, because most gaps on a multicast feed are transient
  // reordering and asking immediately generates recovery traffic for data already in
  // flight: from every receiver on the group at once.
  [[nodiscard]] constexpr result<void> on_gap(sequence_range hole,
                                              time_point now) noexcept {
    std::uint64_t remaining = hole.count();
    std::uint64_t at = hole.first;
    while (remaining > 0) {
      const std::uint64_t take = remaining < policy_.max_messages_per_request
                                     ? remaining
                                     : policy_.max_messages_per_request;
      const sequence_range chunk{.first = at, .end = at + take};
      if (const auto err = admit(chunk, now); !err) DFR_UNLIKELY {
        return err;
      }
      at += take;
      remaining -= take;
    }
    return ok();
  }

  // Records that messages arrived, removing them from what is still being asked for.
  //
  // Splits an entry when the arrival lands in its middle, for the same reason gap_set
  // does, and can therefore run out of room. Refused rather than approximated: an
  // entry covering messages that already arrived would keep asking for them, and one
  // that quietly swallowed the second half would stop asking for messages that never
  // came.
  [[nodiscard]] constexpr result<void> on_filled(
      sequence_range arrived) noexcept {
    if (arrived.empty() || count_ == 0) {
      return ok();
    }

    std::size_t needed = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const remainder left = subtract(entries_[i].range, arrived);
      needed += static_cast<std::size_t>(!left.before.empty()) +
                static_cast<std::size_t>(!left.after.empty());
    }
    if (needed > kMaxOutstandingGaps) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    std::array<entry, kMaxOutstandingGaps> next{};
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const remainder left = subtract(entries_[i].range, arrived);
      // Both halves inherit the attempt state: a partial reply is progress on the
      // same request, not a reason to start counting attempts again.
      if (!left.before.empty()) {
        next[kept] = entries_[i];
        next[kept].range = left.before;
        ++kept;
      }
      if (!left.after.empty()) {
        next[kept] = entries_[i];
        next[kept].range = left.after;
        ++kept;
      }
    }
    entries_ = next;
    count_ = kept;
    return ok();
  }

  // What to do now. Called repeatedly; returns one decision per call so the caller's
  // loop stays flat and so a caller that can only send one request per iteration is
  // not forced to buffer the rest.
  //
  // Abandonment is checked before sending, and the oldest range wins both races. The
  // oldest is the one closest to falling out of the retention window, so spending an
  // attempt on a younger range first is spending it on the one that could have waited.
  [[nodiscard]] constexpr decision poll(time_point now) noexcept {
    if (const decision give_up = take_abandoned(now);
        give_up.what != action::idle) DFR_UNLIKELY {
      return give_up;
    }
    return take_due(now);
  }

 private:
  struct entry {
    sequence_range range{};
    time_point discovered{};
    time_point next_attempt{};
    std::uint32_t attempts{0};
  };

  // Adds one chunk, merging into an entry it touches when the result still fits in a
  // single wire request.
  //
  // A merge resets the attempt count and keeps the *earlier* discovery time, and the
  // split is deliberate: the retention window is a fact about the age of the data, so
  // it belongs to the oldest byte in the range, while the attempt count is a fact
  // about a request, and merging has produced a request nobody has made yet.
  [[nodiscard]] constexpr result<void> admit(sequence_range chunk,
                                             time_point now) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      entry& existing = entries_[i];
      if (!existing.range.overlaps(chunk) && !existing.range.adjacent_to(chunk)) {
        continue;
      }
      const sequence_range joined = merge(existing.range, chunk);
      if (joined.count() > policy_.max_messages_per_request) {
        continue;  // would not fit one request; keep them separate
      }
      existing.range = joined;
      existing.attempts = 0;
      existing.next_attempt = now + policy_.settle_delay;
      if (existing.discovered > now) {
        existing.discovered = now;
      }
      return ok();
    }

    if (count_ >= kMaxOutstandingGaps) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }
    entries_[count_] = entry{.range = chunk,
                             .discovered = now,
                             .next_attempt = now + policy_.settle_delay,
                             .attempts = 0};
    ++count_;
    return ok();
  }

  // The oldest range that can no longer be repaired by asking again.
  [[nodiscard]] constexpr decision take_abandoned(time_point now) noexcept {
    std::size_t worst = count_;
    error reason = error::ok;
    for (std::size_t i = 0; i < count_; ++i) {
      const entry& e = entries_[i];
      const error why = give_up_reason(e, now);
      if (why == error::ok) {
        continue;
      }
      if (worst == count_ || e.discovered < entries_[worst].discovered) {
        worst = i;
        reason = why;
      }
    }
    if (worst == count_) {
      return decision{};
    }

    const sequence_range range = entries_[worst].range;
    remove_at(worst);
    ++stats_.ranges_abandoned;
    stats_.messages_abandoned += range.count();
    if (reason == error::retransmit_window_exceeded) {
      ++stats_.window_exceeded;
    } else {
      ++stats_.timed_out;
    }
    return decision{
        .what = action::abandon, .range = range, .attempt = 0, .reason = reason};
  }

  [[nodiscard]] constexpr error give_up_reason(const entry& e,
                                              time_point now) const noexcept {
    // The window is measured from discovery, not from the last attempt: the
    // publisher's buffer is about the age of the data, not about our persistence.
    if (now - e.discovered >= policy_.retention_window) {
      return error::retransmit_window_exceeded;
    }
    // Every attempt has been made *and* the last one has had its full timeout to
    // arrive. Giving up the instant the last request is sent would abandon a range
    // whose reply was still in flight.
    if (e.attempts >= policy_.max_attempts && now >= e.next_attempt) {
      return error::retransmit_timed_out;
    }
    return error::ok;
  }

  [[nodiscard]] constexpr decision take_due(time_point now) noexcept {
    std::size_t chosen = count_;
    for (std::size_t i = 0; i < count_; ++i) {
      const entry& e = entries_[i];
      if (e.attempts >= policy_.max_attempts || now < e.next_attempt) {
        continue;
      }
      if (chosen == count_ || e.discovered < entries_[chosen].discovered) {
        chosen = i;
      }
    }
    if (chosen == count_) {
      return decision{};
    }

    entry& e = entries_[chosen];
    ++e.attempts;
    e.next_attempt = now + policy_.timeout_for(e.attempts);
    ++stats_.requests_sent;
    return decision{.what = action::send,
                    .range = e.range,
                    .attempt = e.attempts,
                    .reason = error::ok};
  }

  constexpr void remove_at(std::size_t index) noexcept {
    DFR_ASSERT(index < count_, "removing an entry that is not there");
    for (std::size_t i = index + 1; i < count_; ++i) {
      entries_[i - 1] = entries_[i];
    }
    --count_;
  }

  retransmit_policy policy_{};
  requester_stats stats_{};
  std::array<entry, kMaxOutstandingGaps> entries_{};
  std::size_t count_{0};
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_REQUESTER_HPP

// Merging redundant lines into one stream, and noticing when they disagree.
//
// First copy wins, against a watermark and nothing else
// ----------------------------------------------------
// The arbiter holds one number for the merged stream — the highest sequence delivered —
// and decides each packet against it. It does *not* keep a set of delivered ranges, and
// that is a decision rather than a shortcut: gap and reorder bookkeeping already exists
// in gap_tracker, correct and tested, and a second copy inside the arbiter would create
// two answers to one question. The arbiter's job is to make sure each message crosses
// the boundary once; deciding what is still missing belongs downstream.
//
// A watermark is also what production arbitration actually is. The two lines carry the
// same bytes in the same order, offset by the difference in path latency, so "have I
// seen this yet" is answered by one comparison. Out-of-order arrival *within* one line
// is a different problem and has a different owner.
//
// Divergence is a fault, not a vote
// ---------------------------------
// The arbiter never prefers a line when the two disagree. A/B redundancy rests on the
// lines being identical by construction; once they are not, neither copy can be
// trusted, and silently taking whichever arrived first is how a receiver ends up
// confidently wrong. It is reported as error::lines_diverged, which is fatal.
//
// The check needs a digest because the arbiter holds no payloads — it is not a buffer
// and must not become one. The caller supplies whatever it already has, and the history
// is bounded, so a line running more than kDigestHistory packets behind stops being
// checked for divergence. That limit is documented rather than hidden, because a check
// that quietly stops checking is worse than no check.

#ifndef DFR_RECOVERY_ARBITER_HPP
#define DFR_RECOVERY_ARBITER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/gap.hpp>
#include <dfr/recovery/line.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace recovery {

template <clock_source Clock>
class arbiter {
 public:
  using time_point = typename Clock::time_point;

  constexpr arbiter() noexcept = default;

  explicit constexpr arbiter(arbiter_options options) noexcept
      : options_(options) {
    DFR_ASSERT(options_.validate().has_value(),
               "arbiter options must be validated before use");
  }

  // Offers one packet seen on one line.
  //
  // `digest` is any value that differs when the payload differs — a checksum the caller
  // already computes, or the payload length as a weak stand-in. It is only read when
  // options.detect_divergence is set, and a caller that has nothing may pass zero
  // consistently, which disables the check for that stream rather than producing false
  // positives.
  [[nodiscard]] constexpr result<arbitration_result> offer(
      std::size_t line, sequence_range arrived, std::uint64_t digest,
      time_point now) noexcept {
    DFR_ASSERT(line < kMaxLines, "line index out of range");
    line_state& state = lines_[line];
    line_stats& tally = stats_[line];

    ++tally.packets;
    tally.messages += arrived.count();
    state.last_seen = now;
    state.seen = true;
    if (arrived.end > tally.highest_sequence) {
      tally.highest_sequence = arrived.end;
    }

    if (options_.detect_divergence) {
      if (const auto err = check_agreement(arrived, digest); !err) DFR_UNLIKELY {
        return err.error_code();
      }
      remember(arrived, digest);
    }

    // A heartbeat carries no messages, so it can neither be a first copy nor a
    // duplicate. It still counts as a sign of life, which is why the bookkeeping above
    // happens first.
    if (arrived.empty()) {
      return arbitration_result{.outcome = arbitration::duplicate};
    }

    const sequence_range fresh = novel_part(arrived);
    if (fresh.empty()) {
      ++tally.duplicates;
      return arbitration_result{.outcome = arbitration::duplicate};
    }

    ++tally.first_copies;
    if (fresh.end > delivered_through_) {
      delivered_through_ = fresh.end;
    }
    return arbitration_result{
        .outcome = fresh == arrived ? arbitration::deliver : arbitration::partial,
        .deliver = fresh};
  }

  // The highest sequence handed downstream. Everything below it has crossed the
  // boundary exactly once.
  [[nodiscard]] constexpr std::uint64_t delivered_through() const noexcept {
    return delivered_through_;
  }

  [[nodiscard]] constexpr const line_stats& stats(std::size_t line) const noexcept {
    DFR_ASSERT(line < kMaxLines, "line index out of range");
    return stats_[line];
  }

  // Whether a line has been heard from recently enough to be trusted as running.
  //
  // A line that has never been seen is not live, which is the answer that makes a
  // start-up check work: a receiver configured for two lines and wired to one should
  // report the second as down immediately rather than after the first timeout.
  [[nodiscard]] constexpr bool is_live(std::size_t line,
                                       time_point now) const noexcept {
    DFR_ASSERT(line < kMaxLines, "line index out of range");
    const line_state& state = lines_[line];
    if (!state.seen) {
      return false;
    }
    return now - state.last_seen < options_.liveness_timeout;
  }

  // How far behind the leader a line is, in messages.
  //
  // Zero for the leader, and for a line that has never been seen it is the whole
  // stream — which is the honest answer rather than zero, since a silent line has
  // delivered nothing.
  [[nodiscard]] constexpr std::uint64_t messages_behind(
      std::size_t line) const noexcept {
    DFR_ASSERT(line < kMaxLines, "line index out of range");
    const std::uint64_t reached = stats_[line].highest_sequence;
    return reached >= delivered_through_ ? 0 : delivered_through_ - reached;
  }

  // How many configured lines are still running. The number an operator alarms on:
  // losing one line is routine and invisible in the data, and it is the only warning
  // before losing the second is not.
  [[nodiscard]] constexpr std::size_t live_lines(std::size_t configured,
                                                 time_point now) const noexcept {
    DFR_ASSERT(configured <= kMaxLines, "more lines than the arbiter supports");
    std::size_t alive = 0;
    for (std::size_t i = 0; i < configured; ++i) {
      if (is_live(i, now)) {
        ++alive;
      }
    }
    return alive;
  }

 private:
  struct line_state {
    time_point last_seen{};
    bool seen{false};
  };

  struct digest_entry {
    sequence_range range{};
    std::uint64_t digest{0};
  };

  // The part of `arrived` that has not been delivered yet.
  //
  // Only the tail can be new, because the watermark is monotone: anything below it has
  // already crossed. A packet entirely below the watermark is a duplicate, one entirely
  // above is new, and one that straddles it contributes its tail.
  [[nodiscard]] constexpr sequence_range novel_part(
      sequence_range arrived) const noexcept {
    if (arrived.end <= delivered_through_) {
      return sequence_range{};
    }
    const std::uint64_t from = arrived.first > delivered_through_
                                   ? arrived.first
                                   : delivered_through_;
    return sequence_range{.first = from, .end = arrived.end};
  }

  // Reports disagreement with anything remembered that overlaps.
  //
  // Only overlapping ranges are compared, and only when the *same* range is claimed
  // twice is the digest meaningful — two lines splitting the same messages into
  // different packets is legal on some venues, so a digest mismatch across differently
  // framed ranges would be a false alarm. Equality of the range is therefore part of
  // the test, not just overlap.
  [[nodiscard]] constexpr result<void> check_agreement(
      sequence_range arrived, std::uint64_t digest) const noexcept {
    for (std::size_t i = 0; i < digest_count_; ++i) {
      const digest_entry& past = digests_[i];
      if (past.range == arrived && past.digest != digest) DFR_UNLIKELY {
        return error::lines_diverged;
      }
    }
    return ok();
  }

  constexpr void remember(sequence_range arrived, std::uint64_t digest) noexcept {
    if (arrived.empty()) {
      return;
    }
    digests_[digest_next_] = digest_entry{.range = arrived, .digest = digest};
    digest_next_ = (digest_next_ + 1) % kDigestHistory;
    if (digest_count_ < kDigestHistory) {
      ++digest_count_;
    }
  }

  arbiter_options options_{};
  std::uint64_t delivered_through_{0};
  std::array<line_state, kMaxLines> lines_{};
  std::array<line_stats, kMaxLines> stats_{};

  // A ring rather than a growing list, so the divergence check costs a fixed amount
  // however long the feed runs.
  std::array<digest_entry, kDigestHistory> digests_{};
  std::size_t digest_next_{0};
  std::size_t digest_count_{0};
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_ARBITER_HPP

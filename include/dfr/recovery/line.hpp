// The vocabulary of redundant lines: what an offered packet turned out to be, and
// what each line has done for us lately.
//
// A "line" is one physical copy of the same logical feed: CME's A and B channels,
// B3's MBO_EQT_Incremental_FeedA and FeedB. The publisher sends identical bytes down
// both, so arbitration is deduplication and *not* consensus. That distinction is worth
// making explicitly because the vocabulary of consensus is tempting and wrong here:
// there is no quorum, no leader and no split brain. Two lines disagreeing is not a
// vote to be resolved, it is a fault to be reported.
//
// A line index is a plain integer, unlike recovery::channel_id
// ------------------------------------------------------------
// channel_id is a distinct type because the wire carries a channel *number* chosen by
// the venue, and using that number as an array index is a real and silent bug. No such
// number exists for lines: which socket is "A" is a local wiring fact the operator
// configures, and nothing on the wire says it. A strong type would guard against a
// confusion that has nothing to confuse it with, so the index is bounds-asserted and
// left as an integer.

#ifndef DFR_RECOVERY_LINE_HPP
#define DFR_RECOVERY_LINE_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/gap.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace recovery {

// Two is the norm; four leaves room for a venue that publishes more copies without
// making the per-line state large enough to matter.
inline constexpr std::size_t kMaxLines = 4;

// How many recently delivered ranges are remembered for divergence checking.
//
// Bounded, and the bound is a real limitation rather than a formality: a line running
// more than this many packets behind can no longer be checked for divergence, only for
// duplication. Said out loud here because a divergence check that silently stops
// checking is worse than none.
inline constexpr std::size_t kDigestHistory = 64;

enum class arbitration : std::uint8_t {
  // Every message in this packet is new. Pass it downstream.
  deliver,
  // Every message in it has already been delivered by another line, or by this one.
  // The expected outcome for roughly half of all packets on a healthy A/B pair.
  duplicate,
  // Some messages are new and some are not, which happens when the lines are offset
  // by less than a packet. Only the new part is delivered.
  partial,

  count_
};

inline constexpr auto kArbitrationCount =
    static_cast<std::size_t>(arbitration::count_);

[[nodiscard]] constexpr std::string_view name_of(arbitration value) noexcept {
  switch (value) {
    case arbitration::deliver:   return "deliver";
    case arbitration::duplicate: return "duplicate";
    case arbitration::partial:   return "partial";
    case arbitration::count_:    break;
  }
  DFR_UNREACHABLE("unnamed arbitration outcome");
}

struct arbitration_result {
  arbitration outcome{arbitration::duplicate};

  // The portion that is new and should go downstream. Empty for a duplicate, which
  // means a caller can act on this field alone and treat the outcome as commentary.
  sequence_range deliver{};

  // True when this packet was the first copy of at least one message, which is the
  // number that says whether a line is earning its keep.
  [[nodiscard]] constexpr bool won() const noexcept { return !deliver.empty(); }
};

struct line_stats {
  std::uint64_t packets{0};
  std::uint64_t messages{0};
  // Packets that carried at least one message no other line had delivered yet.
  std::uint64_t first_copies{0};
  std::uint64_t duplicates{0};
  // The highest sequence this line has ever reached, so "how far behind is B?" has an
  // answer even when B has stopped sending entirely.
  std::uint64_t highest_sequence{0};

  [[nodiscard]] friend constexpr bool operator==(const line_stats&,
                                                 const line_stats&) = default;
};

struct arbiter_options {
  // How long a line may be silent before it is considered down.
  //
  // Compared against the last packet *seen*, not the last packet that won a race: a
  // healthy line that consistently loses by a microsecond is still healthy, and one
  // that has stopped sending is down even if it was winning every race a second ago.
  duration liveness_timeout{std::chrono::milliseconds{1'000}};

  // Whether to compare content digests for overlapping sequences. Off means a caller
  // that has no cheap digest to hand pays nothing; on means divergence between lines is
  // detected rather than assumed away.
  bool detect_divergence{true};

  [[nodiscard]] constexpr result<void> validate() const noexcept {
    if (liveness_timeout <= duration::zero()) {
      return error::invalid_argument;
    }
    return ok();
  }
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_LINE_HPP

// What one packet did to the receiver's picture of the stream, and the tally.
//
// Separate from gap_tracker.hpp because the audiences differ: a reporting tool, a
// log formatter or a test oracle needs this vocabulary and never needs the tracker
// itself. Splitting it also keeps the tracker file about one thing — the state
// machine — which is the seam docs/STYLE.md §1.10 asks for.

#ifndef DFR_RECOVERY_OBSERVATION_HPP
#define DFR_RECOVERY_OBSERVATION_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/recovery/gap.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace recovery {

// What one packet did to the tracker's picture of the stream.
//
// Not an `error`: three of these are good news, and forcing them through an error
// type would make the caller's success path read as failure handling. The two that
// are also error codes — sequence_gap, session_changed — keep their meaning; a
// caller wanting the error-shaped view asks is_fatal() on the code observe() also
// returns.
enum class sequencing : std::uint8_t {
  // The first packet seen on this channel. Not a gap: a receiver joining a live feed
  // mid-session has missed everything before it and must not report that as a hole
  // it could recover.
  established,
  // Exactly the messages expected, in order. The overwhelming majority.
  in_order,
  // A jump forward. Messages were missed and are now outstanding.
  gap_opened,
  // A packet behind the expected point that covered sequences known to be missing.
  // A retransmit doing its job, or the other side of an A/B pair arriving second.
  gap_filled,
  // A packet behind the expected point covering nothing that was missing. A
  // duplicate. Routine and uninteresting, which is why it is distinguished from
  // gap_filled rather than lumped in with it.
  duplicate,
  // The session identifier changed. Every sequence number held refers to a stream
  // that no longer exists, so the channel's state is discarded.
  session_reset,

  count_
};

inline constexpr auto kSequencingCount = static_cast<std::size_t>(sequencing::count_);

[[nodiscard]] constexpr std::string_view name_of(sequencing value) noexcept {
  switch (value) {
    case sequencing::established:   return "established";
    case sequencing::in_order:      return "in_order";
    case sequencing::gap_opened:    return "gap_opened";
    case sequencing::gap_filled:    return "gap_filled";
    case sequencing::duplicate:     return "duplicate";
    case sequencing::session_reset: return "session_reset";
    case sequencing::count_:        break;
  }
  DFR_UNREACHABLE("unnamed sequencing outcome");
}

// The full account of one packet, because a caller needs more than the category.
//
// A gap_opened caller must know *which* range to request; a gap_filled caller must
// know how much was actually recovered, since a retransmit may overlap a hole only
// partly and may cover sequences nobody was missing.
struct observation {
  sequencing outcome{sequencing::in_order};

  // The range newly discovered missing (gap_opened), or the range this packet
  // covered that had been missing (gap_filled). Empty otherwise.
  sequence_range range{};

  // Non-zero only for gap_filled. Distinct from range.count() when the packet
  // covered sequences that were not missing, which is the normal A/B case.
  std::uint64_t recovered{0};

  // The equivalent error code, for a caller that wants one — is_fatal() applies.
  // `ok` for the outcomes that are not errors at all.
  [[nodiscard]] constexpr error code() const noexcept {
    switch (outcome) {
      case sequencing::gap_opened:    return error::sequence_gap;
      case sequencing::duplicate:     return error::sequence_regressed;
      case sequencing::session_reset: return error::session_changed;
      case sequencing::established:
      case sequencing::in_order:
      case sequencing::gap_filled:    return error::ok;
      case sequencing::count_:        break;
    }
    DFR_UNREACHABLE("observation with no outcome");
  }
};

// Per-channel counters, so a run can be summarised without the caller keeping its
// own tally and so a test can assert on totals rather than on every step.
struct channel_stats {
  std::uint64_t packets{0};
  std::uint64_t messages{0};
  std::uint64_t gaps_opened{0};
  std::uint64_t messages_missed{0};
  std::uint64_t messages_recovered{0};
  std::uint64_t duplicates{0};
  std::uint64_t session_resets{0};
  std::uint64_t messages_abandoned{0};

  [[nodiscard]] friend constexpr bool operator==(const channel_stats&,
                                                 const channel_stats&) = default;
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_OBSERVATION_HPP

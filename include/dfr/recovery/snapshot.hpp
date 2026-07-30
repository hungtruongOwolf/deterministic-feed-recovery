// Deciding whether a snapshot can actually repair the stream.
//
// The whole thing is one pure function, and that is deliberate: this is the single most
// consequential decision in the library, so it is written with no state, no clock and no
// I/O, fully constexpr, and its case analysis is exhaustive rather than defensive.
//
// The mechanism it models
// ----------------------
// NASDAQ's Glimpse is the archetype. The client connects over SoupBinTCP, receives the
// current book as a run of ITCH messages, and the run ends with a marker carrying the
// sequence number of the *next* message the live MoldUDP64 feed will produce. The client
// is meanwhile buffering the live multicast, so recovery is: apply the snapshot, then
// replay the buffer from that sequence number forward. CME's instrument-definition and
// snapshot channels work the same way with different names.
//
// The race, and why it is dangerous rather than merely annoying
// ------------------------------------------------------------
// A snapshot takes time to build and to send. If it comes back reflecting state as of a
// sequence number *earlier* than the oldest message the client managed to buffer, then
// the messages in between exist in neither place. The gap is permanent — no retransmit
// can help, because the client does not know it has one.
//
// That is the failure mode worth building a library around. A client that does not check
// applies the snapshot, replays what it happens to hold, and produces a book that is
// plausible, complete-looking, internally consistent and silently wrong for the rest of
// the session. It will not diverge visibly. It will just be missing some orders.
//
// The other direction is harmless and must not be confused with it: a snapshot *ahead* of
// the buffer covers everything buffered and more, so the buffer is simply discarded.
//
// One practical note that belongs with the mechanism: a client should begin buffering the
// live feed *before* requesting the snapshot. Requesting first widens the window in which
// this race can be lost, and no amount of checking here recovers data nobody kept.

#ifndef DFR_RECOVERY_SNAPSHOT_HPP
#define DFR_RECOVERY_SNAPSHOT_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/recovery/gap.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace recovery {

enum class snapshot_verdict : std::uint8_t {
  // The snapshot can be applied and the buffer replayed on top of it. The ordinary
  // outcome, including the case where the whole buffer turns out to be redundant.
  usable,
  // The snapshot is older than the oldest message buffered, so the range between them is
  // in neither and can never be filled. Everything must be discarded and recovery
  // restarted; there is nothing to salvage and pretending otherwise is the failure this
  // function exists to prevent.
  behind_buffer,
  // Everything the snapshot establishes has already been delivered. Applying it would
  // replace current state with older state. Harmless, and the right response is to throw
  // the snapshot away rather than the stream.
  stale,

  count_
};

inline constexpr auto kSnapshotVerdictCount =
    static_cast<std::size_t>(snapshot_verdict::count_);

[[nodiscard]] constexpr std::string_view name_of(snapshot_verdict value) noexcept {
  switch (value) {
    case snapshot_verdict::usable:        return "usable";
    case snapshot_verdict::behind_buffer: return "behind_buffer";
    case snapshot_verdict::stale:         return "stale";
    case snapshot_verdict::count_:        break;
  }
  DFR_UNREACHABLE("unnamed snapshot verdict");
}

// What to do with a snapshot, spelled out so the caller does no arithmetic of its own.
//
// The two ranges are both returned even though one is derivable from the other, because
// the derivation is where the off-by-one lives. A caller that has to compute "which part
// of my buffer is already in the snapshot" is a caller that will eventually replay a
// message twice or skip one, and either produces a wrong book from correct inputs.
struct snapshot_plan {
  snapshot_verdict verdict{snapshot_verdict::usable};

  // Buffered messages the snapshot already accounts for. Drop these.
  sequence_range discard{};

  // Buffered messages to replay after applying the snapshot, in order.
  sequence_range replay{};

  // The range that exists in neither the snapshot nor the buffer. Non-empty only for
  // behind_buffer, and the number an operator needs: it is exactly how much data this
  // client would have been silently missing.
  sequence_range unfillable{};

  // The sequence the live feed should be expected to continue from once the plan has been
  // carried out. Meaningless for behind_buffer, where there is no valid state to continue
  // from.
  std::uint64_t resume_from{0};

  [[nodiscard]] constexpr error reason() const noexcept {
    switch (verdict) {
      case snapshot_verdict::behind_buffer: return error::snapshot_behind_buffer;
      case snapshot_verdict::stale:         return error::snapshot_stale;
      case snapshot_verdict::usable:        return error::ok;
      case snapshot_verdict::count_:        break;
    }
    DFR_UNREACHABLE("plan with no verdict");
  }

  [[nodiscard]] friend constexpr bool operator==(const snapshot_plan&,
                                                 const snapshot_plan&) = default;
};

// Classifies a snapshot against what the client is holding.
//
// `snapshot_next_sequence` is the sequence of the first message *after* the snapshot's
// state — what Glimpse's end-of-snapshot marker carries. Off-by-one here inverts the
// whole result, so it is named for what it is rather than "snapshot sequence".
//
// `already_delivered` is the watermark of what has gone downstream, or zero on a cold
// start.
//
// The order of the checks is load-bearing. Staleness comes first because if the stream
// has already progressed past the snapshot, what the buffer holds is beside the point.
// The behind-buffer check comes next because it is the one that must never be reached
// *after* something has been discarded on the assumption that the snapshot was usable.
[[nodiscard]] constexpr snapshot_plan plan_snapshot(
    std::uint64_t snapshot_next_sequence, sequence_range buffered,
    std::uint64_t already_delivered = 0) noexcept {
  if (snapshot_next_sequence <= already_delivered) {
    return snapshot_plan{.verdict = snapshot_verdict::stale,
                         .resume_from = already_delivered};
  }

  if (buffered.empty()) {
    // Nothing buffered is not a problem: the snapshot establishes state through
    // snapshot_next_sequence - 1 and the live feed continues from there. It is only a
    // problem if the client also failed to *start* buffering in time, which is a fact
    // this function cannot see and the caller must not infer from `usable`.
    return snapshot_plan{.verdict = snapshot_verdict::usable,
                         .resume_from = snapshot_next_sequence};
  }

  if (snapshot_next_sequence < buffered.first) {
    return snapshot_plan{
        .verdict = snapshot_verdict::behind_buffer,
        .unfillable = sequence_range{.first = snapshot_next_sequence,
                                     .end = buffered.first},
        .resume_from = 0};
  }

  if (snapshot_next_sequence >= buffered.end) {
    // The snapshot covers everything buffered and possibly more. The buffer is redundant
    // rather than dangerous, and nothing is replayed.
    return snapshot_plan{.verdict = snapshot_verdict::usable,
                         .discard = buffered,
                         .resume_from = snapshot_next_sequence};
  }

  // The ordinary overlap: the snapshot lands inside what is buffered.
  return snapshot_plan{
      .verdict = snapshot_verdict::usable,
      .discard = sequence_range{.first = buffered.first,
                                .end = snapshot_next_sequence},
      .replay = sequence_range{.first = snapshot_next_sequence,
                               .end = buffered.end},
      .resume_from = buffered.end};
}

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_SNAPSHOT_HPP

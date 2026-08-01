// What a receiver is missing, per channel, and what each arriving packet did to it.
//
// The difference from wire::iextp::chain_checker is the point of this file. A checker
// *detects*: it reports the first inconsistency and resynchronises, holding no memory
// of what was lost. A tracker *remembers*: it records the hole, keeps accepting live
// data past it, and later recognises the retransmit that fills it. Detection is one
// return value; recovery is state that has to survive being wrong.
//
// Protocol-agnostic on purpose. It takes a session, a first sequence number and a
// message count(the three things MoldUDP64 and IEX-TP both provide) so the
// dependency runs from wire adapters into recovery and never back. A tracker that
// took an iextp::header would need a second copy for MoldUDP64 and a third for the
// in-memory simulator.
//
// No network I/O, no callbacks. Copied deliberately from itchcpp's scope sentence,
// which is the part of that library to imitate: *"The library never performs network
// I/O itself: when SequenceTracker sees a gap it calls request_retransmit, and the
// caller is responsible for issuing the actual re-request."* Here even the call is
// gone: observe() returns what happened and the caller decides, which is
// TIGER_STYLE's *"don't do things directly in reaction to external events"*.

#ifndef DFR_RECOVERY_GAP_TRACKER_HPP
#define DFR_RECOVERY_GAP_TRACKER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/channel.hpp>
#include <dfr/recovery/gap.hpp>
#include <dfr/recovery/gap_set.hpp>
#include <dfr/recovery/observation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::recovery {

class gap_tracker {
 public:
  constexpr gap_tracker() noexcept = default;

  // Feeds one packet's sequencing fields.
  //
  // A heartbeat(count 0) is deliberately not a special case here. It carries the
  // sequence number of the *next* message, so it can legitimately open a gap while
  // delivering nothing, and treating it specially would lose that. The one thing it
  // must not do is advance the expectation past itself, which falls out of the
  // arithmetic rather than needing a branch: first + 0 == first.
  [[nodiscard]] constexpr result<observation> observe(
      channel_id id, std::uint32_t session, std::uint64_t first_sequence,
      std::uint64_t message_count) noexcept {
    state& channel = channels_[id.index()];
    channel_stats& tally = stats_[id.index()];
    ++tally.packets;
    tally.messages += message_count;

    const sequence_range arrived{.first = first_sequence,
                                 .end = first_sequence + message_count};

    if (!channel.started) {
      channel.started = true;
      channel.session = session;
      channel.expected = arrived.end;
      return observation{.outcome = sequencing::established};
    }

    if (session != channel.session) DFR_UNLIKELY {
      // Everything held refers to the old session, including the outstanding holes:
      // requesting a retransmit of them would be asking the new session for
      // sequence numbers that mean something else there.
      tally.messages_abandoned += channel.missing.total_missing();
      channel.missing.clear();
      channel.session = session;
      channel.expected = arrived.end;
      ++tally.session_resets;
      return observation{.outcome = sequencing::session_reset};
    }

    if (first_sequence == channel.expected) {
      channel.expected = arrived.end;
      return observation{.outcome = sequencing::in_order};
    }

    if (first_sequence > channel.expected) {
      return open_gap(channel, tally, arrived);
    }
    return handle_behind(channel, tally, arrived);
  }

  // Tells the tracker a snapshot re-established state at `sequence`, so holes below
  // it can never be filled.
  //
  // The session is a parameter and not an oversight to be fixed later: a snapshot
  // facility identifies the session it is a snapshot *of*: Glimpse replies carry it,
  // CME's instrument-definition channel carries it, and a snapshot is the one thing a
  // receiver may have before it has seen a single live packet. Without it, recovering
  // first and then joining the feed makes the first live packet look like a session
  // change, which is fatal, so the receiver would discard the snapshot it just
  // successfully applied.
  //
  // The abandoned count is returned rather than logged, because giving up on a hole
  // is data loss even when correct, and a receiver that cannot say how much it gave
  // up cannot be audited. This is the accounting the Glimpse race destroys when it
  // goes unnoticed: a snapshot behind the buffer produces a plausible book that is
  // permanently missing messages nobody counted.
  [[nodiscard]] constexpr std::uint64_t snapshot_at(
      channel_id id, std::uint32_t session, std::uint64_t sequence) noexcept {
    state& channel = channels_[id.index()];
    channel_stats& tally = stats_[id.index()];

    // A snapshot of a different session supersedes everything held, holes included:
    // those sequence numbers mean something else in the new session.
    if (channel.started && session != channel.session) DFR_UNLIKELY {
      const std::uint64_t abandoned = channel.missing.total_missing();
      tally.messages_abandoned += abandoned;
      channel.missing.clear();
      channel.session = session;
      channel.expected = sequence;
      ++tally.session_resets;
      return abandoned;
    }

    const std::uint64_t abandoned = channel.missing.discard_below(sequence);
    tally.messages_abandoned += abandoned;
    channel.session = session;
    if (!channel.started || sequence > channel.expected) {
      channel.started = true;
      channel.expected = sequence;
    }
    return abandoned;
  }

  [[nodiscard]] constexpr bool started(channel_id id) const noexcept {
    return channels_[id.index()].started;
  }

  [[nodiscard]] constexpr std::uint64_t expected_sequence(
      channel_id id) const noexcept {
    return channels_[id.index()].expected;
  }

  [[nodiscard]] constexpr const gap_set& outstanding(
      channel_id id) const noexcept DFR_LIFETIME_BOUND {
    return channels_[id.index()].missing;
  }

  [[nodiscard]] constexpr const channel_stats& stats(
      channel_id id) const noexcept DFR_LIFETIME_BOUND {
    return stats_[id.index()];
  }

  // Missing across every channel, which is the number an operator watches.
  [[nodiscard]] constexpr std::uint64_t total_missing() const noexcept {
    std::uint64_t total = 0;
    for (const state& channel : channels_) {
      total += channel.missing.total_missing();
    }
    return total;
  }

 private:
  struct state {
    gap_set missing{};
    std::uint64_t expected{0};
    std::uint32_t session{0};
    bool started{false};
  };

  [[nodiscard]] static constexpr result<observation> open_gap(
      state& channel, channel_stats& tally, sequence_range arrived) noexcept {
    const sequence_range hole{.first = channel.expected, .end = arrived.first};
    if (const auto err = channel.missing.open(hole); !err) DFR_UNLIKELY {
      // Out of room to describe what is missing. The expectation is deliberately
      // *not* advanced: advancing would mean the tracker had accepted this packet
      // and forgotten the hole in the same step, and the next packet would then look
      // in-order while a range had vanished from the accounting.
      return err.error_code();
    }
    channel.expected = arrived.end;
    ++tally.gaps_opened;
    tally.messages_missed += hole.count();
    return observation{.outcome = sequencing::gap_opened, .range = hole};
  }

  // A packet at or below the expected sequence: a retransmit, an A/B twin, or a
  // plain duplicate. Which one it is depends on whether it covers anything missing,
  // so the gap set is the thing that decides rather than the sequence number alone.
  [[nodiscard]] static constexpr result<observation> handle_behind(
      state& channel, channel_stats& tally, sequence_range arrived) noexcept {
    std::uint64_t recovered = 0;
    if (const auto err = channel.missing.fill(arrived).get(recovered);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }

    // A packet can start behind the expectation and end past it: a retransmit that
    // also carries fresh messages. The tail beyond the expectation is new and
    // contiguous, so the expectation advances and no gap opens.
    if (arrived.end > channel.expected) {
      channel.expected = arrived.end;
    }

    if (recovered == 0) {
      ++tally.duplicates;
      return observation{.outcome = sequencing::duplicate};
    }
    tally.messages_recovered += recovered;
    return observation{.outcome = sequencing::gap_filled,
                       .range = arrived,
                       .recovered = recovered};
  }

  std::array<state, kMaxChannels> channels_{};
  std::array<channel_stats, kMaxChannels> stats_{};
};

}  // namespace dfr::inline v1::recovery
#endif  // DFR_RECOVERY_GAP_TRACKER_HPP

// A recovery client: arbiter → gap_tracker → requester, with a replay_buffer standing by for
// the snapshot path, given one order and one state.
//
// Performs no I/O and reads no clock: poll() says what to send and the caller sends it, time
// arrives as an argument, and nothing allocates after construction.
//
// Three composition decisions are argued in docs/DESIGN.md §7b rather than here — one client
// per channel, a message-granular replay buffer under packet-granular sequencing, and why the
// arbiter's position and the tracker's expectation are kept in step explicitly.

#ifndef DFR_RECOVERY_CLIENT_HPP
#define DFR_RECOVERY_CLIENT_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/arbiter.hpp>
#include <dfr/recovery/channel.hpp>
#include <dfr/recovery/client_state.hpp>
#include <dfr/recovery/gap.hpp>
#include <dfr/recovery/gap_set.hpp>
#include <dfr/recovery/gap_tracker.hpp>
#include <dfr/recovery/replay_buffer.hpp>
#include <dfr/recovery/requester.hpp>
#include <dfr/recovery/snapshot.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace recovery {

template <clock_source Clock, typename Buffer = replay_buffer<>>
class client {
 public:
  using time_point = typename Clock::time_point;

  explicit constexpr client(client_options options) noexcept
      : options_(options),
        arbiter_(options.arbitration),
        requester_(options.retransmission) {
    DFR_ASSERT(options_.validate().has_value(),
               "client options must be validated before use");
  }

  [[nodiscard]] constexpr client_state state() const noexcept { return state_; }

  // The highest sequence actually handed downstream — deliberately not the arbiter's
  // watermark, which means "seen on the merged stream". See docs/DESIGN.md §7b.
  [[nodiscard]] constexpr std::uint64_t delivered_through() const noexcept {
    return delivered_through_;
  }
  [[nodiscard]] constexpr const arbiter<Clock>& arbitration() const noexcept
      DFR_LIFETIME_BOUND {
    return arbiter_;
  }
  [[nodiscard]] constexpr const gap_tracker& tracking() const noexcept
      DFR_LIFETIME_BOUND {
    return tracker_;
  }
  [[nodiscard]] constexpr const requester<Clock>& retransmission() const noexcept
      DFR_LIFETIME_BOUND {
    return requester_;
  }
  [[nodiscard]] constexpr const Buffer& held() const noexcept DFR_LIFETIME_BOUND {
    return buffer_;
  }
  [[nodiscard]] constexpr std::uint64_t total_missing() const noexcept {
    return tracker_.total_missing();
  }

  // Exactly which sub-ranges the last offered packet repaired.
  //
  // Held by the client and handed back by reference rather than copied into every report:
  // a packet can repair several separate holes at once, so the answer is a set, and a set
  // in a per-packet return value would be a few hundred bytes moved for the overwhelming
  // majority of packets that repair nothing.
  [[nodiscard]] constexpr const gap_set& last_recovered() const noexcept
      DFR_LIFETIME_BOUND {
    return last_recovered_;
  }
  [[nodiscard]] constexpr std::size_t live_lines(time_point now) const noexcept {
    return arbiter_.live_lines(options_.lines, now);
  }

  // Offers one packet, from one line.
  //
  // The arbiter runs first, so a duplicate never reaches the tracker: feeding both copies
  // of every packet into sequence tracking would report half the stream as regressed and
  // make the duplicate count look like an error rate.
  [[nodiscard]] constexpr result<ingest_report> on_packet(
      std::size_t line, std::uint32_t session, std::uint64_t first_sequence,
      std::uint64_t message_count, std::uint64_t digest,
      time_point now) noexcept {
    if (state_ == client_state::failed) DFR_UNLIKELY {
      return failure_;
    }
    if (state_ == client_state::replaying) DFR_UNLIKELY {
      // The caller owes us finish_replay(). Accepting a packet now would deliver it ahead
      // of messages that are already known and already older.
      return error::invalid_argument;
    }

    // A session change invalidates every number held, including the arbiter's watermark
    // and the holes being chased. Handled before arbitration rather than after, because
    // the watermark from the old session would classify the new session's first packets
    // as duplicates and the client would sit silent forever.
    if (started_ && session != session_) DFR_UNLIKELY {
      restart_for_new_session(session);
    }

    const sequence_range arrived{.first = first_sequence,
                                 .end = first_sequence + message_count};
    arbitration_result merged;
    if (const auto err = arbiter_.offer(line, arrived, digest, now).get(merged);
        err != error::ok) DFR_UNLIKELY {
      state_ = client_state::failed;
      failure_ = err;
      return err;
    }

    started_ = true;
    session_ = session;

    ingest_report report{.merge = merged.outcome, .accepted = merged.deliver};

    // "Is this a duplicate?" and "is this useful?" are different questions, and the
    // arbiter can only answer the first. A retransmit — or the other line's copy arriving
    // late — lands below the merged watermark and looks like a duplicate while being
    // exactly what recovery was waiting for. The holes are the only thing that can tell
    // them apart, so they are consulted here, before observe() closes them.
    last_recovered_ = tracker_.outstanding(kChannel).intersect(arrived);
    report.recovered = last_recovered_.total_missing();

    // A heartbeat carries no messages, so there is nothing to deliver and nothing to
    // repair — but the sequence number it carries is how a receiver learns its position
    // during a quiet period. Returning here would mean a jump announced by a heartbeat was
    // never noticed, and on IEX two thirds of all packets are heartbeats, so that is not a
    // corner case but the common one.
    const bool carries_messages = message_count > 0;
    if (carries_messages && merged.deliver.empty() && last_recovered_.empty()) {
      // Nothing new and nothing repaired: a plain duplicate. Returning before the tracker
      // sees it is what keeps a redundant pair from reporting half the stream as regressed.
      return report;
    }

    // The whole arrived range, not just the part above the watermark: the tracker owns the
    // holes and has to see the part below it in order to close them.
    observation seen;
    if (const auto err = tracker_
                             .observe(kChannel, session, arrived.first,
                                      arrived.count())
                             .get(seen);
        err != error::ok) DFR_UNLIKELY {
      // The tracker ran out of room to describe what is missing, which means the client
      // has lost track rather than merely lost data. A snapshot is the only way back.
      escalate(err);
      return err;
    }
    report.outcome = seen.outcome;

    // Keep the arbiter's position in step with the tracker's expectation, explicitly.
    //
    // They must agree, and they do not always move together: a heartbeat carries no
    // messages, so the arbiter's watermark cannot advance on it, while the tracker's
    // expectation does — and on IEX two thirds of packets are heartbeats. Once the two
    // disagree, a hole can sit *above* the watermark, and the retransmit that fills it then
    // counts as both new and repaired. That is one message delivered twice, which corrupts a
    // book exactly as thoroughly as losing one.
    //
    // With the two in step the disjointness is a theorem rather than a coincidence: every
    // hole is below the tracker's expectation, `accepted` starts at the watermark, and the
    // watermark equals the expectation. The paranoid assertion below states it so that a
    // future change which breaks the invariant fails loudly rather than double-delivering.
    arbiter_.adopt(tracker_.expected_sequence(kChannel));
    DFR_ASSERT_PARANOID(
        merged.deliver.empty() || last_recovered_.empty() ||
            last_recovered_.ranges().back().end <= merged.deliver.first,
        "a message counted as both newly arrived and newly repaired");

    if (seen.outcome == sequencing::gap_opened) {
      report.gap_opened = seen.range;
      if (const auto err = requester_.on_gap(seen.range, now); !err) DFR_UNLIKELY {
        escalate(err.error_code());
        return err.error_code();
      }
    } else if (seen.outcome == sequencing::gap_filled && seen.recovered > 0) {
      if (const auto err = requester_.on_filled(seen.range); !err) DFR_UNLIKELY {
        escalate(err.error_code());
        return err.error_code();
      }
    }

    if (state_ == client_state::synchronising) {
      state_ = client_state::live;
    }
    report.held_for_replay = state_ == client_state::recovering;
    if (!report.held_for_replay && merged.deliver.end > delivered_through_) {
      delivered_through_ = merged.deliver.end;
    }
    return report;
  }

  // Hands over one message the report said was held for replay. Separate from on_packet()
  // for the reason argued in docs/DESIGN.md §7b.
  [[nodiscard]] constexpr result<void> buffer_message(std::uint64_t sequence,
                                                      packet_view message) noexcept {
    DFR_ASSERT(state_ == client_state::recovering,
               "messages are only buffered while recovering");
    if (const auto err = buffer_.append(sequence, message); !err) DFR_UNLIKELY {
      // Either the buffer filled or the messages arrived with a hole. Both mean this
      // recovery attempt cannot produce a correct book, and neither can be papered over:
      // see the note at the top of replay_buffer.hpp.
      state_ = client_state::failed;
      failure_ = err.error_code();
      return err;
    }
    return ok();
  }

  // Records that the venue refused a retransmit request.
  //
  // Without it, a client whose data has aged out of the publisher's window would spend every
  // remaining attempt asking a facility that has already said the messages are gone, reaching a
  // snapshot late by timing out instead of promptly by being told. A fatal reason escalates at
  // once; a transient one is left to the retry schedule, since "busy, try later" is not evidence
  // that the data is unrecoverable.
  [[nodiscard]] constexpr result<void> on_retransmit_refused(
      sequence_range range, error reason) noexcept {
    if (state_ == client_state::failed) DFR_UNLIKELY {
      return failure_;
    }
    if (is_fatal(reason)) DFR_UNLIKELY {
      escalate(reason, range);
    }
    return ok();
  }

  // What to do now.
  //
  // Failure is reported first and repeatedly: a client that has given up must keep saying
  // so, because a caller that polls once and ignores the answer would otherwise see idle
  // and assume everything was fine.
  [[nodiscard]] constexpr client_decision poll(time_point now) noexcept {
    if (state_ == client_state::failed) DFR_UNLIKELY {
      return client_decision{.what = client_action::restart, .reason = failure_};
    }
    if (state_ == client_state::replaying) {
      // Waiting on the caller, not on the network.
      return client_decision{};
    }
    if (state_ == client_state::recovering) {
      // Nothing to ask the retransmit server for: the snapshot supersedes every
      // outstanding hole, and asking anyway would spend the retention window on ranges
      // about to be discarded.
      return client_decision{.what = client_action::request_snapshot,
                             .range = pending_snapshot_range_,
                             .reason = snapshot_reason_};
    }

    const decision next = requester_.poll(now);
    switch (next.what) {
      case action::send:
        return client_decision{.what = client_action::send_retransmit_request,
                               .range = next.range,
                               .attempt = next.attempt};
      case action::abandon:
        // Retransmission has given up. Escalating to a snapshot is the only honest
        // response; continuing to stream past a hole that can never be filled means
        // publishing a book known to be wrong.
        escalate(next.reason, next.range);
        return client_decision{.what = client_action::request_snapshot,
                               .range = next.range,
                               .reason = next.reason};
      case action::idle:
      case action::count_:
        break;
    }
    return client_decision{};
  }

  // Applies the outcome of a snapshot request.
  //
  // Returns the plan so the caller can act on it — which messages to discard and which to
  // replay — rather than the client silently doing something with data it does not own. Takes no
  // time argument: whether a snapshot is usable depends only on sequence numbers.
  [[nodiscard]] constexpr result<snapshot_plan> on_snapshot(
      std::uint32_t session, std::uint64_t snapshot_next_sequence) noexcept {
    if (state_ == client_state::failed) DFR_UNLIKELY {
      return failure_;
    }

    const snapshot_plan plan = plan_snapshot(snapshot_next_sequence,
                                             buffer_.buffered(),
                                             delivered_through());
    switch (plan.verdict) {
      case snapshot_verdict::stale:
        // Keep going on live data; the snapshot simply arrived too late to be worth
        // applying. The client stays in recovery, so poll() will ask again.
        return plan;

      case snapshot_verdict::behind_buffer:
        state_ = client_state::failed;
        failure_ = error::snapshot_behind_buffer;
        return plan;

      case snapshot_verdict::usable:
        break;
      case snapshot_verdict::count_:
        DFR_UNREACHABLE("snapshot plan with no verdict");
    }

    // Trim the buffer to exactly what the plan says to replay, and leave it there. The
    // caller has not replayed anything yet, so clearing here would destroy the messages
    // that sit on top of the snapshot — the one part of recovery that cannot be fetched
    // again.
    if (plan.replay.empty()) {
      buffer_.clear();
    } else {
      (void)buffer_.drop_below(plan.replay.first);
    }

    (void)tracker_.snapshot_at(kChannel, session, snapshot_next_sequence);
    // The arbiter's position has to be re-established, or the replayed messages and
    // everything after them would look new a second time. resume_from, not the snapshot's
    // sequence: once the caller has replayed the buffer, everything below resume_from has
    // been delivered.
    arbiter_.reset_stream();
    arbiter_.adopt(plan.resume_from);
    delivered_through_ = plan.resume_from;

    session_ = session;
    started_ = true;
    pending_snapshot_range_ = sequence_range{};
    snapshot_reason_ = error::ok;
    state_ = plan.replay.empty() ? client_state::live : client_state::replaying;
    return plan;
  }

  // Says the buffered messages have been handed downstream.
  //
  // Required rather than implied, because until it is called the client cannot know
  // whether the caller managed to replay them, and offering fresh packets in the meantime
  // would deliver messages out of order.
  [[nodiscard]] constexpr result<void> finish_replay() noexcept {
    if (state_ != client_state::replaying) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    buffer_.clear();
    state_ = client_state::live;
    return ok();
  }

 private:
  // A single channel per client, so the multi-channel tracker is used at index zero.
  static constexpr channel_id kChannel = channel_id::at(0);

  constexpr void escalate(error reason,
                          sequence_range about = sequence_range{}) noexcept {
    state_ = client_state::recovering;
    snapshot_reason_ = reason;
    pending_snapshot_range_ = about;
    buffer_.clear();
  }

  constexpr void restart_for_new_session(std::uint32_t session) noexcept {
    arbiter_.reset_stream();
    buffer_.clear();
    requester_ = requester<Clock>{options_.retransmission};
    pending_snapshot_range_ = sequence_range{};
    snapshot_reason_ = error::ok;
    session_ = session;
    delivered_through_ = 0;
    state_ = client_state::synchronising;
  }

  client_options options_{};
  arbiter<Clock> arbiter_;
  gap_tracker tracker_{};
  requester<Clock> requester_;
  Buffer buffer_{};

  client_state state_{client_state::synchronising};
  error failure_{error::ok};
  error snapshot_reason_{error::ok};
  gap_set last_recovered_{};
  std::uint64_t delivered_through_{0};
  sequence_range pending_snapshot_range_{};
  std::uint32_t session_{0};
  bool started_{false};
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_CLIENT_HPP

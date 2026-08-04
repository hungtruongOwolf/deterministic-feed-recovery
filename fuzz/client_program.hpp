// Driving the recovery client with a program decoded from the input.
//
// Every operation here is one a real caller performs, and the fuzzer's job is to find an order of them that
// breaks something. Nothing malformed is offered on purpose: a client that mishandles a malformed packet is a
// decoder bug, and there are seven targets for that already. What is being explored is the state machine.
//
// The step cap is not a safety net, it is part of the contract. A fuzz input has to reproduce in bounded time or
// the crash report is useless, and an input that ran for a million steps would take a million to shrink.

#ifndef DFR_FUZZ_CLIENT_PROGRAM_HPP
#define DFR_FUZZ_CLIENT_PROGRAM_HPP

#include "client_invariants.hpp"
#include "program.hpp"

#include <chrono>

namespace dfr_fuzz {

inline constexpr std::size_t kMaxSteps = 512;
inline constexpr std::uint64_t kMaxBatch = 8;

inline rec::client_options fuzz_options() noexcept {
  rec::client_options options;
  options.lines = 2;
  options.arbitration.liveness_timeout = std::chrono::milliseconds{100};
  options.retransmission.settle_delay = std::chrono::milliseconds{1};
  options.retransmission.first_timeout = std::chrono::milliseconds{5};
  options.retransmission.max_timeout = std::chrono::milliseconds{50};
  options.retransmission.max_attempts = 3;
  options.retransmission.retention_window = std::chrono::seconds{1};
  return options;
}

// Offers one packet the way a real caller does, including the part callers get wrong.
//
// When the client is replaying a snapshot it does not deliver an arriving packet, it reports `held_for_replay`
// and the caller owes it `buffer_message` for each sequence. Skipping that is not a fuzzing shortcut, it is the
// caller bug the assertion in buffer_message() exists to catch: the first version of this program called it
// whenever it felt like it and tripped that assertion, which is the API stating its precondition, not a defect.
[[nodiscard]] inline bool offer_packet(fuzz_client& client, std::size_t line, const venue_model& venue,
                                       std::uint64_t first, std::uint64_t count,
                                       dfr::manual_clock::time_point now) noexcept {
  rec::ingest_report report;
  if (client.on_packet(line, venue.session, first, count, 0, now).get(report) != dfr::error::ok) {
    return false;
  }
  if (!report.held_for_replay) {
    return true;
  }
  const std::uint8_t body[1]{0};
  for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
    if (!client.buffer_message(s, dfr::packet_view{body, sizeof body})) {
      return true;
    }
  }
  return true;
}

// One run: build a client, decode the input as operations, check the invariants after each.
inline void run_client_program(dfr::packet_view input) noexcept {
  program_reader reader{input};
  venue_model venue;
  fuzz_client client{fuzz_options()};
  history seen;
  auto now = dfr::manual_clock::time_point{};

  // The last range the venue published, so a retransmit can answer something real rather than a number the
  // fuzzer invented. A retransmit for a range never sent is a different test, and it belongs in the unit tests
  // where the expected error can be named.
  rec::sequence_range last_hole{};

  for (std::size_t step = 0; step < kMaxSteps && !reader.done(); ++step) {
    // Whether the client took anything in this step. A packet offered while a snapshot is replaying is refused
    // before on_packet even looks at the session, so a session change can sit unabsorbed for several steps and
    // the watermark resets later than the venue's own switch. The oracle has to follow the client, not the venue.
    bool accepted = false;
    switch (op_at(reader.byte())) {
      case op::publish_in_order: {
        const std::uint64_t count = 1 + reader.upto(kMaxBatch);
        const std::uint64_t first = venue.publish(count);
        accepted = offer_packet(client, 0, venue, first, count, now) || accepted;
        break;
      }
      case op::publish_after_loss: {
        // The venue sends, the network eats it, the next packet reveals the hole.
        const std::uint64_t lost = 1 + reader.upto(kMaxBatch);
        const std::uint64_t lost_first = venue.publish(lost);
        last_hole = rec::sequence_range{.first = lost_first, .end = lost_first + lost};
        const std::uint64_t count = 1 + reader.upto(kMaxBatch);
        const std::uint64_t first = venue.publish(count);
        accepted = offer_packet(client, 0, venue, first, count, now) || accepted;
        break;
      }
      case op::publish_duplicate: {
        // The same packet twice, which is what an A/B pair produces on every packet that is not lost.
        const std::uint64_t count = 1 + reader.upto(kMaxBatch);
        const std::uint64_t first = venue.publish(count);
        accepted = offer_packet(client, 0, venue, first, count, now) || accepted;
        accepted = offer_packet(client, 1, venue, first, count, now) || accepted;
        break;
      }
      case op::publish_on_second_line: {
        const std::uint64_t count = 1 + reader.upto(kMaxBatch);
        const std::uint64_t first = venue.publish(count);
        accepted = offer_packet(client, 1, venue, first, count, now) || accepted;
        break;
      }
      case op::publish_overlapping: {
        // A packet that repeats part of what has already been seen and adds new messages after it.
        //
        // This is the `partial` arbitration path, and it did not exist in the first version of this program: every
        // packet was either wholly new or wholly a duplicate, so the branch that trims an overlap was never
        // executed. A planted defect proved it, by surviving fifty thousand programs. Real feeds produce this
        // constantly, at every retransmit boundary and whenever a redundant line is a little behind.
        const std::uint64_t back = 1 + reader.upto(4);
        const std::uint64_t count = 1 + reader.upto(kMaxBatch);
        const std::uint64_t fresh_first = venue.publish(count);
        const std::uint64_t first = fresh_first > back ? fresh_first - back : 1;
        accepted = offer_packet(client, 0, venue, first, count + (fresh_first - first), now) || accepted;
        break;
      }
      case op::publish_out_of_session: {
        // A session change invalidates every number the client holds. The most destructive legal event on a
        // feed, and the one a client is most likely to half-handle.
        venue.reset_session(venue.session + 1);
        const std::uint64_t count = 1 + reader.upto(kMaxBatch);
        const std::uint64_t first = venue.publish(count);
        accepted = offer_packet(client, 0, venue, first, count, now) || accepted;
        break;
      }
      case op::deliver_retransmit: {
        // A repair arrives as a packet on the line it was requested over, because that is how a retransmit
        // facility answers: same framing, same entry point, and the client works out that it fills a hole.
        if (last_hole.empty()) {
          break;
        }
        accepted = offer_packet(client, 0, venue, last_hole.first, last_hole.count(), now) || accepted;
        last_hole = rec::sequence_range{};
        break;
      }
      case op::refuse_retransmit: {
        if (last_hole.empty()) {
          break;
        }
        (void)client.on_retransmit_refused(last_hole, dfr::error::retransmit_rejected);
        last_hole = rec::sequence_range{};
        break;
      }
      case op::advance_clock: {
        // Milliseconds, bounded, so the retransmit timer is crossed often rather than jumped over.
        now += std::chrono::milliseconds{1 + reader.upto(60)};
        break;
      }
      case op::poll: {
        const auto decision = client.poll(now);
        check_decision(client, decision);
        break;
      }
      case op::deliver_snapshot: {
        // Anywhere from far behind the client to the venue's current position, because a snapshot arriving at
        // the wrong moment is the Glimpse race and it is the reason this path exists.
        //
        // Never *ahead* of the venue, which was the first thing this fuzzer caught and it caught the model rather
        // than the library: a snapshot at a sequence the venue has not reached is not a thing a venue can serve,
        // and offering one made the client's watermark pass the highest published number, exactly as instructed.
        const std::uint64_t at = 1 + reader.upto(static_cast<std::uint32_t>(venue.next_sequence));
        (void)client.on_snapshot(venue.session, at);
        break;
      }
      case op::finish_replay: {
        (void)client.finish_replay();
        break;
      }
      case op::count_:
        break;
    }

    check(client, venue, seen, accepted);
  }
}

}  // namespace dfr_fuzz

#endif  // DFR_FUZZ_CLIENT_PROGRAM_HPP

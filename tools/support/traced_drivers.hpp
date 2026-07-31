// The two runs the trace tool can record.
//
// Split from traced_pipeline.hpp, which is the loop, so this file is only about *which* run is being
// driven. The Glimpse driver in particular is a choreography rather than a loop, and it reads better
// on its own.

#ifndef DFR_TOOLS_SUPPORT_TRACED_DRIVERS_HPP
#define DFR_TOOLS_SUPPORT_TRACED_DRIVERS_HPP

#include "support/traced_pipeline.hpp"

#include <map>
#include <string>

#include <cstdint>
#include <vector>

namespace dfr_tools {

// ---------------------------------------------------------------------------
// The two drivers
// ---------------------------------------------------------------------------

// Loads the facility with everything the venue published, so a retransmit hands back the original
// bytes. Heartbeats are ignored by record() itself, since no request can be answered with one.
inline void load_facility(ven::retransmit_facility<512>& facility,
                          const std::vector<traced_packet>& stream) {
  for (const auto& packet : stream) {
    (void)facility.record(packet.first_sequence, packet.message_count,
                          dfr::packet_view{packet.bytes.data(), packet.bytes.size()});
  }
}

// The ordinary run: faults are injected and the facility repairs them.
inline run_summary run_recovering(const run_options& options,
                                  const std::vector<traced_packet>& stream,
                                  trace_recorder& into,
                               const std::map<std::uint64_t, std::string>* bodies) {
  chaos::schedule plan;
  dfr::prng rng{options.seed};
  chaos::schedule_options schedule_options;
  schedule_options.max_faults = options.faults;
  schedule_options.permitted = traceable_faults();
  (void)chaos::schedule::generate(rng, schedule_options, stream.size()).get(plan);

  ven::retransmit_facility<512> facility;
  load_facility(facility, stream);

  // One injector per line, with different seeds: the whole point of a redundant pair is that the
  // two lines lose *different* packets, so a schedule shared between them would arrange for the
  // redundancy to be useless and the trace would show a feed with no benefit from its second line.
  chaos::schedule other = plan;
  if (options.lines > 1) {
    dfr::prng other_rng{options.seed ^ 0x9E37'79B9'7F4A'7C15ULL};
    (void)chaos::schedule::generate(other_rng, schedule_options, stream.size())
        .get(other);
  }
  chaos::injector<chaos::iextp_target> line_a{plan};
  chaos::injector<chaos::iextp_target> line_b{other};
  traced_pipeline pipeline{options, into};
  pipeline.set_bodies(bodies);

  const auto emit_on = [&](std::size_t line) {
    return [&, line](const chaos::emission& emitted) {
      pipeline.advance(10);
      pipeline.set_index(emitted.source_index);
      pipeline.set_line(line);
      pipeline.note_fault(emitted);
      pipeline.offer(emitted.packet);
      pipeline.answer(facility);
    };
  };

  const auto emit_a = emit_on(0);
  const auto emit_b = emit_on(1);
  for (std::uint64_t i = 0; i < stream.size(); ++i) {
    const dfr::packet_view view{stream[i].bytes.data(), stream[i].bytes.size()};
    if (!line_a.offer(view, i, emit_a)) {
      break;
    }
    if (options.lines > 1 && !line_b.offer(view, i, emit_b)) {
      break;
    }
  }
  (void)line_a.flush(emit_a);
  if (options.lines > 1) {
    (void)line_b.flush(emit_b);
  }
  pipeline.set_line(0);

  // Keep polling after the stream ends: a hole revealed by the last packet is not due to be
  // requested until the settle delay has passed, and there are no further packets to prompt a poll.
  for (int round = 0; round < 64; ++round) {
    pipeline.advance(200);
    pipeline.answer(facility);
  }

  pipeline.finish();
  run_summary out = pipeline.summary();
  out.schedule.assign(plan.faults().begin(), plan.faults().end());
  return out;
}

// The run that loses the Glimpse race, arranged deliberately.
//
// The choreography is exact and it is a fact about the failure rather than about this code: the
// snapshot must land *above* what the client has already delivered — otherwise it is merely stale
// and discarding it is safe — and *below* the oldest message the client managed to buffer. That
// interval only exists when packets are lost after the client enters recovery, so packets are
// dropped both before escalation and after it.
inline run_summary run_glimpse(const run_options& options,
                               const std::vector<traced_packet>& stream,
                               trace_recorder& into,
                               const std::map<std::uint64_t, std::string>* bodies) {
  // A hand-written schedule rather than a seed: a run that has to reach one specific state names
  // the fault it needs instead of hunting for a seed that happens to produce it.
  chaos::schedule plan;
  (void)plan.add(chaos::fault{.op = chaos::fault_op::drop,
                              .first_packet = 8,
                              .packet_count = 2});

  ven::retransmit_facility<512> facility;  // loaded, but never consulted in this mode
  load_facility(facility, stream);
  ven::snapshot_options snapshot_options;
  snapshot_options.session = kTraceSession;
  snapshot_options.latency = std::chrono::microseconds{50};
  snapshot_options.staleness_messages = options.staleness_messages;
  ven::snapshot_facility<trace_clock> snapshots{snapshot_options};

  chaos::injector<chaos::iextp_target> injector{plan};
  traced_pipeline pipeline{options, into};
  pipeline.set_bodies(bodies);

  const std::size_t escalate_by = stream.size() / 4;
  const auto emit = [&](const chaos::emission& emitted) {
    pipeline.advance(20);
    pipeline.set_index(emitted.source_index);
    pipeline.note_fault(emitted);
    pipeline.offer(emitted.packet);
    pipeline.answer(facility);  // asks, is never answered, and eventually gives up
  };

  std::size_t i = 0;
  for (; i < escalate_by; ++i) {
    const dfr::packet_view view{stream[i].bytes.data(), stream[i].bytes.size()};
    if (!injector.offer(view, i, emit)) {
      break;
    }
    snapshots.advance_to(stream[i].first_sequence + stream[i].message_count);
    if (pipeline.client().state() == rec::client_state::recovering) {
      ++i;
      break;
    }
  }
  // Let the requester exhaust its attempts if it has not already.
  for (int round = 0; round < 64 &&
                      pipeline.client().state() != rec::client_state::recovering;
       ++round) {
    pipeline.advance(200);
    pipeline.answer(facility);
  }

  // The feed runs on unseen while the snapshot is built: this is the loss that opens the interval.
  const std::size_t unseen_through = i + 8 < stream.size() ? i + 8 : stream.size() - 1;
  for (std::size_t j = i; j <= unseen_through; ++j) {
    snapshots.advance_to(stream[j].first_sequence + stream[j].message_count);
  }

  pipeline.advance(20);
  pipeline.record(trc::event_kind::snapshot_requested);
  (void)snapshots.request(at_us(pipeline.now_us()));

  // The client catches up from here, buffering for replay.
  const std::size_t buffer_from = unseen_through + 1;
  for (std::size_t j = buffer_from; j < buffer_from + 5 && j < stream.size(); ++j) {
    pipeline.advance(20);
    pipeline.set_index(j);
    pipeline.offer(dfr::packet_view{stream[j].bytes.data(), stream[j].bytes.size()});
  }

  pipeline.advance(200);
  ven::snapshot_reply reply;
  if (snapshots.poll(at_us(pipeline.now_us())).get(reply) == dfr::error::ok) {
    pipeline.record(trc::event_kind::snapshot_replied,
                    rec::sequence_range{.first = reply.next_sequence,
                                        .end = reply.next_sequence},
                    dfr::error::ok, 0, options.staleness_messages);

    rec::snapshot_plan applied;
    if (pipeline.client()
            .on_snapshot(reply.session, reply.next_sequence)
            .get(applied) == dfr::error::ok) {
      const bool refused = applied.verdict == rec::snapshot_verdict::behind_buffer;
      pipeline.record(refused ? trc::event_kind::snapshot_rejected
                              : trc::event_kind::snapshot_applied,
                      refused ? applied.unfillable : applied.replay,
                      applied.reason());
      pipeline.mutable_summary().unfillable_messages = applied.unfillable.count();
    }
  }

  pipeline.advance(20);
  pipeline.record(trc::event_kind::state_changed);
  pipeline.finish();
  run_summary out = pipeline.summary();
  out.unfillable_messages = pipeline.summary().unfillable_messages;
  out.schedule.assign(plan.faults().begin(), plan.faults().end());
  return out;
}

inline run_summary run_traced(const run_options& options,
                              const std::vector<traced_packet>& stream,
                              trace_recorder& into,
                              const std::map<std::uint64_t, std::string>* bodies = nullptr) {
  return options.mode == run_mode::glimpse ? run_glimpse(options, stream, into, bodies)
                                           : run_recovering(options, stream, into, bodies);
}



}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_TRACED_DRIVERS_HPP

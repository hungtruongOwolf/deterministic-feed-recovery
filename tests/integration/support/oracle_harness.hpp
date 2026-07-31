// The pipeline the oracle drives, and the ground truth it checks against.
//
// Two test files use it — detection and repair — so it lives here rather than being copied,
// per docs/STYLE.md §1.10.
//
// The harness predicts nothing. It records which packets it managed to decode and hand over,
// and the oracle compares the client's accounting against that record. A harness that computed
// "a drop of packet 12 should produce a hole of three messages" would be reimplementing the
// injector, and the two would then agree by construction rather than by being right.

#ifndef DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_HARNESS_HPP
#define DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_HARNESS_HPP

#include "integration/support/injected_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <set>
#include <vector>

namespace dfr_test::integration {


using oracle_client = rec::client<dfr::manual_clock, rec::replay_buffer<4096, 512>>;
using oracle_time = dfr::manual_clock::time_point;

inline oracle_time at_us(std::int64_t micros) {
  return oracle_time{} + std::chrono::microseconds{micros};
}

// Hands one packet to the client, if it survives decoding, and records what happened.
//
// A packet that fails to decode or to frame is *discarded*, exactly as a real receiver
// would discard it. That is the harness's whole model of corruption: it never inspects the
// fault that caused it.
inline void offer_if_intact(oracle_client& client, ledger& record,
                     dfr::packet_view packet, oracle_time now) {
  iex::header header;
  if (iex::decode_header(packet).get(header) != dfr::error::ok) {
    ++record.packets_discarded;
    return;
  }
  if (!iex::verify_payload_framing(packet)) {
    ++record.packets_discarded;
    return;
  }

  rec::ingest_report report;
  if (client
          .on_packet(0, header.session, header.first_sequence,
                     header.message_count, 0, now)
          .get(report) != dfr::error::ok) {
    record.client_left_live = true;
    return;
  }
  ++record.packets_offered;

  for (std::uint64_t s = header.first_sequence;
       s < header.first_sequence + header.message_count; ++s) {
    record.offered.insert(s);
  }

  if (!report.delivered()) {
    return;
  }
  for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
    ++record.deliveries[s];
  }
  for (const auto& repaired : client.last_recovered().ranges()) {
    for (std::uint64_t s = repaired.first; s < repaired.end; ++s) {
      ++record.deliveries[s];
    }
  }
}

// The retransmit server: hands back the *undamaged* source packets covering a range.
//
// Undamaged on purpose. A retransmit facility is a different path from the multicast feed —
// TCP for MoldUDP64, TCP for Glimpse — so modelling it as lossy as well would be testing
// two things at once and would make a failure ambiguous.
inline void serve_retransmit(oracle_client& client, ledger& record,
                      const std::vector<source_packet>& stream,
                      rec::sequence_range wanted, oracle_time now) {
  for (const auto& packet : stream) {
    const rec::sequence_range carried{
        .first = packet.first_sequence,
        .end = packet.first_sequence + packet.message_count};
    if (!carried.overlaps(wanted)) {
      continue;
    }
    ++record.retransmits_served;
    offer_if_intact(client, record,
                    dfr::packet_view{packet.bytes.data(), packet.bytes.size()},
                    now);
  }
}

// Answers everything the client is currently asking for.
//
// Bounded rather than "until idle": an unbounded loop would turn a client that asked for the
// same range forever into a hung test instead of a failing one.
inline void drain(oracle_client& client, ledger& record,
           const std::vector<source_packet>& stream, std::int64_t now_us) {
  for (int step = 0; step < 64; ++step) {
    const auto decision = client.poll(at_us(now_us));
    if (decision.what == rec::client_action::idle) {
      return;
    }
    if (decision.what != rec::client_action::send_retransmit_request) {
      record.client_left_live = true;
      return;
    }
    serve_retransmit(client, record, stream, decision.range, at_us(now_us));
  }
}

// Keeps polling after the stream has ended.
//
// Without this, a hole revealed by the very last packet is requested and never served: the
// request is not due until the settle delay has passed, and there are no further packets to
// prompt another poll. A real client's loop keeps running; a test that stops at the last
// packet is testing something the caller would never do.
inline void drain_to_quiet(oracle_client& client, ledger& record,
                    const std::vector<source_packet>& stream,
                    std::int64_t& now_us) {
  for (int round = 0; round < 64; ++round) {
    now_us += 200;
    const auto before = record.retransmits_served;
    drain(client, record, stream, now_us);
    if (record.retransmits_served == before) {
      return;
    }
  }
}

struct run_result {
  ledger record;
  std::uint64_t missing_at_end{0};
  std::uint64_t low{0};
  // One past the highest sequence the stream announced, which is where the accounting stops.
  std::uint64_t high{0};
  std::vector<rec::sequence_range> holes;
  chaos::injection_stats injected;
};

// Runs one seed end to end. `serve` decides whether a retransmit server exists.
inline run_result run(std::uint64_t seed, std::size_t packets, bool serve) {
  const auto stream = clean_stream(packets);

  chaos::schedule plan;
  dfr::prng rng{seed};
  chaos::schedule_options options;
  options.max_faults = 6;  // few enough that the outstanding-hole capacity is not the test
  options.permitted = derivable_faults();
  REQUIRE(chaos::schedule::generate(rng, options, packets).get(plan) ==
          dfr::error::ok);

  chaos::injector<chaos::iextp_target> injector{plan};
  oracle_client client{oracle_options()};
  run_result out;

  std::int64_t clock_us = 0;
  const auto emit = [&](const chaos::emission& e) {
    clock_us += 20;  // past the settle delay, so a hole becomes a request promptly
    offer_if_intact(client, out.record, e.packet, at_us(clock_us));

    if (serve) {
      drain(client, out.record, stream, clock_us);
    }
  };

  for (std::uint64_t i = 0; i < stream.size(); ++i) {
    const auto& packet = stream[i];
    const dfr::packet_view view{packet.bytes.data(), packet.bytes.size()};
    REQUIRE(injector.offer(view, i, emit).has_value());
  }
  REQUIRE(injector.flush(emit).has_value());
  if (serve) {
    drain_to_quiet(client, out.record, stream, clock_us);
  }

  out.injected = injector.stats();
  out.missing_at_end = client.total_missing();
  // The tracker's expectation, not the delivered watermark. A heartbeat announces the
  // sequence of the next message without delivering anything, so the stream can announce a
  // position above what has been delivered — and a hole between the two is a real hole, for
  // messages that never arrived. Checking only the delivered prefix would leave exactly that
  // hole outside the span and call the client wrong for reporting it.
  out.high = client.tracking().expected_sequence(rec::channel_id::at(0));
  out.low = out.record.deliveries.empty() ? 0
                                         : out.record.deliveries.begin()->first;
  const auto& holes = client.tracking().outstanding(rec::channel_id::at(0));
  out.holes.assign(holes.ranges().begin(), holes.ranges().end());
  return out;
}

// Every sequence the client says is still missing.
inline std::set<std::uint64_t> missing_sequences(const run_result& result) {
  std::set<std::uint64_t> out;
  for (const auto& hole : result.holes) {
    for (std::uint64_t s = hole.first; s < hole.end; ++s) {
      out.insert(s);
    }
  }
  return out;
}

// Every sequence in the observed span that the harness never managed to hand over. This is
// ground truth, derived from what happened rather than from what was scheduled.
inline std::set<std::uint64_t> never_offered(const run_result& result) {
  std::set<std::uint64_t> out;
  for (std::uint64_t s = result.low; s < result.high; ++s) {
    if (!result.record.offered.contains(s)) {
      out.insert(s);
    }
  }
  return out;
}

}  // namespace dfr_test::integration

#endif  // DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_HARNESS_HPP

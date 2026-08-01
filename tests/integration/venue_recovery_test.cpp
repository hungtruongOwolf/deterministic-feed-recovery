// A client against a retransmit facility that has a real retention window.
//
// The oracle tests drive the client with a harness that always has the answer. This drives it
// against a facility that forgets, and therefore against a facility that can refuse: a path
// that existed in the client as a unit-tested branch and had never been reached the way it is
// reached in production. Snapshots are in venue_snapshot_test.cpp.

#include "integration/support/venue_rig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;
namespace rec = dfr::recovery;
namespace ven = dfr::venue;

using dfr_test::venue_rig::at_us;
using dfr_test::venue_rig::kSession;
using dfr_test::venue_rig::offer;
using dfr_test::venue_rig::publish;
using dfr_test::venue_rig::published;
using dfr_test::venue_rig::rig_client;
using dfr_test::venue_rig::rig_client_options;
using dfr_test::venue_rig::rig_ledger;
using dfr_test::venue_rig::rig_publisher;
using dfr_test::venue_rig::rig_publisher_options;

// ---------------------------------------------------------------------------
// A venue that can answer
// ---------------------------------------------------------------------------

TEST_CASE("a client recovers through a real retransmit facility",
          "[integration][venue]") {
  // Same claim as the oracle(every injected fault repaired, every message once) but the
  // server is now a facility with a bounded window rather than a harness with the whole day in
  // memory.
  rig_publisher publisher{rig_publisher_options()};
  const auto stream = publish(publisher, 600);

  ven::retransmit_facility<512> facility;
  for (const auto& packet : stream) {
    REQUIRE(facility
                .record(packet.first_sequence, packet.message_count,
                        dfr::packet_view{packet.bytes.data(), packet.bytes.size()})
                .has_value());
  }

  chaos::schedule plan;
  dfr::prng rng{31};
  chaos::schedule_options schedule_options;
  schedule_options.max_faults = 4;
  chaos::op_mask mask;
  mask.disable(chaos::fault_op::flip_bit);
  mask.disable(chaos::fault_op::rewrite_sequence);
  mask.disable(chaos::fault_op::rewrite_session);
  schedule_options.permitted = mask;
  REQUIRE(chaos::schedule::generate(rng, schedule_options, stream.size()).get(plan) ==
          dfr::error::ok);

  chaos::injector<chaos::iextp_target> injector{plan};
  rig_client client{rig_client_options()};
  rig_ledger record;
  std::int64_t now = 0;

  const auto answer = [&]() {
    for (int step = 0; step < 32; ++step) {
      const auto decision = client.poll(at_us(now));
      if (decision.what != rec::client_action::send_retransmit_request) {
        break;
      }
      ++record.retransmit_requests;
      const auto served = facility.serve(
          decision.range, [&](dfr::packet_view packet) {
            offer(client, record, packet, at_us(now));
          });
      if (!served) {
        ++record.retransmit_refusals;
        REQUIRE(client.on_retransmit_refused(decision.range, served.error_code())
                    .has_value());
        break;
      }
    }
  };

  const auto emit = [&](const chaos::emission& e) {
    now += 20;
    offer(client, record, e.packet, at_us(now));
    answer();
  };

  for (std::uint64_t i = 0; i < stream.size(); ++i) {
    const dfr::packet_view view{stream[i].bytes.data(), stream[i].bytes.size()};
    REQUIRE(injector.offer(view, i, emit).has_value());
  }
  REQUIRE(injector.flush(emit).has_value());
  for (int round = 0; round < 64; ++round) {
    now += 200;
    answer();
  }

  CHECK(client.state() == rec::client_state::live);
  CHECK(client.total_missing() == 0);
  CHECK(record.delivered_twice().empty());
  CHECK(record.retransmit_requests > 0);
  CHECK(record.retransmit_refusals == 0);
  CHECK(facility.stats().messages_served > 0);
}

// ---------------------------------------------------------------------------
// A venue that says the data is gone
// ---------------------------------------------------------------------------

TEST_CASE("a refusal sends the client to a snapshot instead of timing out",
          "[integration][venue]") {
  // The API the venue exposed. Without it the client would spend every remaining attempt asking
  // a facility that had already said the messages were gone, and would reach a snapshot late by
  // timing out rather than promptly by being told.
  rig_publisher publisher{rig_publisher_options()};
  const auto stream = publish(publisher, 300);

  // A window far too small to hold the whole stream, so an early hole has certainly aged out.
  ven::retransmit_facility<4> facility;
  for (const auto& packet : stream) {
    REQUIRE(facility
                .record(packet.first_sequence, packet.message_count,
                        dfr::packet_view{packet.bytes.data(), packet.bytes.size()})
                .has_value());
  }
  REQUIRE(facility.evicted() > 0);

  rig_client client{rig_client_options()};
  rig_ledger record;
  std::int64_t now = 0;

  // Feed the first few packets, skip one, then carry on: the hole is old by the time it is asked
  // about, because the facility has already moved past it.
  for (std::size_t i = 0; i < 20; ++i) {
    if (i == 5) {
      continue;  // dropped in transit
    }
    now += 20;
    offer(client, record, dfr::packet_view{stream[i].bytes.data(),
                                          stream[i].bytes.size()},
          at_us(now));
  }
  REQUIRE(client.total_missing() > 0);

  const auto asked = client.poll(at_us(now));
  REQUIRE(asked.what == rec::client_action::send_retransmit_request);

  const auto served = facility.serve(asked.range, [](dfr::packet_view) {});
  REQUIRE_FALSE(served.has_value());
  CHECK(served.error_code() == dfr::error::retransmit_window_exceeded);
  CHECK(facility.stats().refused_too_old == 1);

  REQUIRE(client.on_retransmit_refused(asked.range, served.error_code())
              .has_value());
  CHECK(client.state() == rec::client_state::recovering);
  CHECK(client.poll(at_us(now)).what == rec::client_action::request_snapshot);

  // And it did not burn its attempts to get there.
  CHECK(client.retransmission().stats().requests_sent == 1);
}

TEST_CASE("a transient refusal does not abandon a repairable gap",
          "[integration][venue]") {
  // "Busy, try later" is not evidence that the data is unrecoverable, and treating it as such
  // would send a client to a snapshot over a hiccup.
  rig_publisher publisher{rig_publisher_options()};
  const auto stream = publish(publisher, 60);

  rig_client client{rig_client_options()};
  rig_ledger record;
  std::int64_t now = 0;
  for (std::size_t i = 0; i < 10; ++i) {
    if (i == 4) {
      continue;
    }
    now += 20;
    offer(client, record, dfr::packet_view{stream[i].bytes.data(),
                                          stream[i].bytes.size()},
          at_us(now));
  }
  const auto asked = client.poll(at_us(now));
  REQUIRE(asked.what == rec::client_action::send_retransmit_request);

  REQUIRE(client.on_retransmit_refused(asked.range, dfr::error::retransmit_rejected)
              .has_value());
  CHECK(client.state() == rec::client_state::live);

  // And the repair still lands when it is offered.
  now += 20;
  offer(client, record, dfr::packet_view{stream[4].bytes.data(),
                                        stream[4].bytes.size()},
        at_us(now));
  CHECK(client.total_missing() == 0);
}


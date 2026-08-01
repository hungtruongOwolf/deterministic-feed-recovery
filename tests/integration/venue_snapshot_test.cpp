// The snapshot path, end to end: winning the Glimpse race and losing it.
//
// Both outcomes are driven through a whole client against a facility that captures its position
// when the request arrives, which is what Glimpse does and the only version that can fail.
// recovery::plan_snapshot's behind_buffer verdict stops being a unit-tested branch here.

#include "integration/support/venue_rig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace rec = dfr::recovery;
namespace ven = dfr::venue;

using dfr_test::venue_rig::at_us;
using dfr_test::venue_rig::carried_by;
using dfr_test::venue_rig::kSession;
using dfr_test::venue_rig::offer;
using dfr_test::venue_rig::publish;
using dfr_test::venue_rig::published;
using dfr_test::venue_rig::rig_client;
using dfr_test::venue_rig::rig_client_options;
using dfr_test::venue_rig::rig_ledger;
using dfr_test::venue_rig::rig_publisher;
using dfr_test::venue_rig::rig_publisher_options;

namespace {

using rig_snapshot = ven::snapshot_facility<dfr_test::venue_rig::rig_clock>;

// Drives a client to the point where it is asking for a snapshot, using the venue's own refusal
// rather than a timeout.
//
// The facility is advanced for *every* published packet, including the one the client never
// receives: the venue published it, and a snapshot facility knows where the feed is rather than
// where any particular client has got to. Driving both from the same index is what made an
// earlier version of this test classify every snapshot as stale: the venue was never actually
// ahead of the client, which is the only situation in which the race can exist.
void drive_to_snapshot(rig_client& client, rig_ledger& record,
                       const std::vector<published>& stream,
                       rig_snapshot& snapshots, std::int64_t& now) {
  ven::retransmit_facility<4> facility;
  for (const auto& packet : stream) {
    REQUIRE(facility
                .record(packet.first_sequence, packet.message_count,
                        dfr::packet_view{packet.bytes.data(), packet.bytes.size()})
                .has_value());
  }

  for (std::size_t i = 0; i < 20; ++i) {
    now += 20;
    snapshots.advance_to(carried_by(stream[i]).end);
    if (i == 5) {
      continue;  // lost in transit; the facility has long since moved past it
    }
    offer(client, record, dfr::packet_view{stream[i].bytes.data(),
                                          stream[i].bytes.size()},
          at_us(now));
  }
  REQUIRE(client.total_missing() > 0);

  const auto asked = client.poll(at_us(now));
  REQUIRE(asked.what == rec::client_action::send_retransmit_request);
  const auto served = facility.serve(asked.range, [](dfr::packet_view) {});
  REQUIRE_FALSE(served.has_value());
  REQUIRE(client.on_retransmit_refused(asked.range, served.error_code())
              .has_value());
  REQUIRE(client.state() == rec::client_state::recovering);
}

// Advances the venue's feed without the client seeing any of it: packets still being lost while
// the client waits for its snapshot.
void publish_unseen(rig_snapshot& snapshots, const std::vector<published>& stream,
                    std::size_t from, std::size_t through) {
  for (std::size_t i = from; i <= through; ++i) {
    snapshots.advance_to(carried_by(stream[i]).end);
  }
}

// Offers packets the client does manage to receive while recovering, which it buffers for replay.
void buffer_range(rig_client& client, rig_ledger& record,
                  const std::vector<published>& stream, std::size_t from,
                  std::size_t through, std::int64_t& now) {
  for (std::size_t i = from; i <= through; ++i) {
    now += 20;
    offer(client, record, dfr::packet_view{stream[i].bytes.data(),
                                          stream[i].bytes.size()},
          at_us(now));
  }
}

// Hands the buffered messages downstream and tells the client they are gone.
void complete_replay(rig_client& client, rig_ledger& record) {
  const auto held = client.held().buffered();
  for (std::uint64_t s = held.first; s < held.end; ++s) {
    ++record.deliveries[s];
  }
  REQUIRE(client.finish_replay().has_value());
  ++record.replays_finished;
}

ven::snapshot_options snapshot_options_with(std::uint64_t staleness) {
  ven::snapshot_options options;
  options.session = kSession;
  options.latency = std::chrono::microseconds{50};
  options.staleness_messages = staleness;
  REQUIRE(options.validate().has_value());
  return options;
}

}  // namespace

TEST_CASE("a snapshot taken in time completes recovery",
          "[integration][venue]") {
  rig_publisher publisher{rig_publisher_options()};
  const auto stream = publish(publisher, 300);

  rig_snapshot snapshots{snapshot_options_with(0)};
  rig_client client{rig_client_options()};
  rig_ledger record;
  std::int64_t now = 0;
  drive_to_snapshot(client, record, stream, snapshots, now);

  // The feed is ahead of the client, which is the ordinary state of affairs and the reason the
  // snapshot's position is worth checking at all.
  publish_unseen(snapshots, stream, 20, 27);

  REQUIRE(client.poll(at_us(now)).what == rec::client_action::request_snapshot);
  ++record.snapshot_requests;
  REQUIRE(snapshots.request(at_us(now)).has_value());

  // While the snapshot is built the client keeps buffering, and here it manages to catch
  // packets that straddle the position the snapshot froze at.
  buffer_range(client, record, stream, 25, 29, now);
  const auto buffered = client.held().buffered();
  REQUIRE_FALSE(buffered.empty());

  now += 100;
  ven::snapshot_reply reply;
  REQUIRE(snapshots.poll(at_us(now)).get(reply) == dfr::error::ok);
  REQUIRE(reply.next_sequence >= buffered.first);  // the race was won

  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(reply.session, reply.next_sequence).get(plan) ==
          dfr::error::ok);
  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.unfillable.empty());
  ++record.snapshots_applied;

  if (client.state() == rec::client_state::replaying) {
    complete_replay(client, record);
  }
  CHECK(client.state() == rec::client_state::live);
  CHECK(client.total_missing() == 0);  // the abandoned hole is gone for good

  // And the stream carries on in order, which is the only proof the repair actually worked.
  now += 20;
  rec::ingest_report report;
  REQUIRE(client
              .on_packet(0, kSession, stream[30].first_sequence,
                         stream[30].message_count, 0, at_us(now))
              .get(report) == dfr::error::ok);
  CHECK(report.outcome == rec::sequencing::in_order);
  CHECK(client.total_missing() == 0);
}

TEST_CASE("a snapshot from a lagging replica loses the Glimpse race",
          "[integration][venue]") {
  // The failure this project was chosen for, driven through a whole client for the first time.
  //
  // The mechanism is exact, and it is the reason the test has to be built this way: the snapshot
  // must land *above* what the client has already delivered: otherwise it is merely stale and
  // discarding it is safe, and *below* the oldest message the client managed to buffer. That
  // interval only exists when packets were lost after the client entered recovery, so this test
  // loses packets 20 through 24 and starts buffering at 25.
  //
  // A client that did not check would apply the snapshot, replay what it happened to hold, and
  // publish a book that is plausible, internally consistent and permanently missing the messages
  // in between. This one refuses.
  rig_publisher publisher{rig_publisher_options()};
  const auto stream = publish(publisher, 300);

  // Served from a replica well behind the live feed.
  rig_snapshot snapshots{snapshot_options_with(20)};
  rig_client client{rig_client_options()};
  rig_ledger record;
  std::int64_t now = 0;
  drive_to_snapshot(client, record, stream, snapshots, now);
  const std::uint64_t delivered_before = client.delivered_through();

  publish_unseen(snapshots, stream, 20, 27);

  REQUIRE(client.poll(at_us(now)).what == rec::client_action::request_snapshot);
  REQUIRE(snapshots.request(at_us(now)).has_value());

  // Packets 20..24 are lost as well; the client only starts buffering at 25.
  buffer_range(client, record, stream, 25, 29, now);
  const auto buffered = client.held().buffered();
  REQUIRE_FALSE(buffered.empty());

  now += 100;
  ven::snapshot_reply reply;
  REQUIRE(snapshots.poll(at_us(now)).get(reply) == dfr::error::ok);
  REQUIRE(reply.next_sequence > delivered_before);   // not merely stale
  REQUIRE(reply.next_sequence < buffered.first);     // the race has been lost

  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(reply.session, reply.next_sequence).get(plan) ==
          dfr::error::ok);
  CHECK(plan.verdict == rec::snapshot_verdict::behind_buffer);
  CHECK(plan.unfillable ==
        rec::sequence_range{.first = reply.next_sequence, .end = buffered.first});
  CHECK(plan.unfillable.count() > 0);

  // Nothing is salvaged, and the client keeps saying so rather than carrying on.
  CHECK(client.state() == rec::client_state::failed);
  for (const std::int64_t later : {now + 100, now + 10'000}) {
    const auto verdict = client.poll(at_us(later));
    CHECK(verdict.what == rec::client_action::restart);
    CHECK(verdict.reason == dfr::error::snapshot_behind_buffer);
  }
}

// Escalation to a snapshot, replaying on top of it, and the failures that have no repair.
//
// This is the path the whole library exists for, driven end to end through one object.

#include <dfr/recovery/client.hpp>

#include "recovery/support/client_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rec = dfr::recovery;

using dfr_test::recovery::at_ms;
using dfr_test::recovery::bytes_of;
using dfr_test::recovery::hand_over;
using dfr_test::recovery::kLineA;
using dfr_test::recovery::kSession;
using dfr_test::recovery::offer;
using dfr_test::recovery::range;
using dfr_test::recovery::readable_options;
using dfr_test::recovery::test_client;

namespace {

// Drives a client to the point where retransmission has given up on 6..10 and a snapshot
// is being asked for. Used by most tests below, because getting there takes a timeline
// rather than a call.
test_client abandoned_client() {
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 6), at_ms(0));
  offer(client, kLineA, range(11, 16), at_ms(0));  // 6..10 missing

  // Three attempts, then the last one's timeout.
  REQUIRE(client.poll(at_ms(1)).attempt == 1);
  REQUIRE(client.poll(at_ms(11)).attempt == 2);
  REQUIRE(client.poll(at_ms(31)).attempt == 3);

  const auto escalated = client.poll(at_ms(71));
  REQUIRE(escalated.what == rec::client_action::request_snapshot);
  REQUIRE(escalated.reason == dfr::error::retransmit_timed_out);
  REQUIRE(client.state() == rec::client_state::recovering);
  return client;
}

}  // namespace

// ---------------------------------------------------------------------------
// Escalation
// ---------------------------------------------------------------------------

TEST_CASE("retransmission giving up escalates to a snapshot",
          "[recovery][client]") {
  // The only honest response. Continuing to stream past a hole that can never be filled
  // means publishing a book known to be wrong, which is the failure this project is about.
  auto client = abandoned_client();
  CHECK(client.state() == rec::client_state::recovering);
}

TEST_CASE("the snapshot request is repeated until it is answered",
          "[recovery][client]") {
  // A caller that polls and drops the answer must be told again. Reporting idle here would
  // leave a client stuck in recovery with nobody fetching anything.
  auto client = abandoned_client();
  for (const std::int64_t t : {72, 100, 500, 5'000}) {
    CHECK(client.poll(at_ms(t)).what == rec::client_action::request_snapshot);
  }
}

TEST_CASE("no retransmit requests go out while recovering",
          "[recovery][client]") {
  // The snapshot supersedes every outstanding hole, so asking for them anyway would spend
  // the retention window on ranges about to be discarded.
  auto client = abandoned_client();
  const auto sent_before = client.retransmission().stats().requests_sent;

  offer(client, kLineA, range(16, 21), at_ms(80));
  offer(client, kLineA, range(30, 35), at_ms(81));  // a fresh hole, during recovery
  for (const std::int64_t t : {82, 200, 900}) {
    CHECK(client.poll(at_ms(t)).what == rec::client_action::request_snapshot);
  }
  CHECK(client.retransmission().stats().requests_sent == sent_before);
}

TEST_CASE("live data is held for replay while recovering",
          "[recovery][client]") {
  // Not delivered and not discarded. Until the snapshot's position is known there is no
  // way to tell which of these messages it already accounts for.
  auto client = abandoned_client();

  const auto held = offer(client, kLineA, range(16, 20), at_ms(80));
  CHECK(held.held_for_replay);
  CHECK_FALSE(held.delivered());
  CHECK(held.accepted == range(16, 20));

  hand_over(client, held.accepted);
  CHECK(client.held().buffered() == range(16, 20));
}

// ---------------------------------------------------------------------------
// Applying a snapshot
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot inside the buffer leaves exactly the replay set",
          "[recovery][client]") {
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(16, 24), at_ms(80));
  hand_over(client, held.accepted);
  REQUIRE(client.held().buffered() == range(16, 24));

  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 20).get(plan) == dfr::error::ok);

  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.discard == range(16, 20));
  CHECK(plan.replay == range(20, 24));
  CHECK(client.state() == rec::client_state::replaying);
  // The buffer is trimmed to the replay set and *kept*: clearing it here would destroy the
  // one part of recovery that cannot be fetched again.
  CHECK(client.held().buffered() == range(20, 24));
}

TEST_CASE("finishing the replay puts the client back live",
          "[recovery][client]") {
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(16, 24), at_ms(80));
  hand_over(client, held.accepted);
  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 20).get(plan) == dfr::error::ok);

  REQUIRE(client.finish_replay().has_value());
  CHECK(client.state() == rec::client_state::live);
  CHECK(client.held().buffered().empty());
  CHECK(client.delivered_before() == 24);
  CHECK(client.total_missing() == 0);  // the abandoned hole is gone for good
}

TEST_CASE("the stream continues from where the replay ended",
          "[recovery][client]") {
  // The end-to-end property: after a snapshot and a replay, the next live packet is in
  // order rather than a gap or a duplicate.
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(16, 24), at_ms(80));
  hand_over(client, held.accepted);
  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 20).get(plan) == dfr::error::ok);
  REQUIRE(client.finish_replay().has_value());

  const auto next = offer(client, kLineA, range(24, 28), at_ms(100));
  CHECK(next.outcome == rec::sequencing::in_order);
  CHECK(next.delivered());
  CHECK(client.total_missing() == 0);
}

TEST_CASE("a snapshot covering the whole buffer needs no replay",
          "[recovery][client]") {
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(16, 20), at_ms(80));
  hand_over(client, held.accepted);

  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 100).get(plan) == dfr::error::ok);
  CHECK(plan.replay.empty());
  CHECK(client.state() == rec::client_state::live);  // straight back, no replay step
  CHECK(client.delivered_before() == 100);
}

TEST_CASE("offering a packet while replaying is refused",
          "[recovery][client]") {
  // The caller owes finish_replay(). Accepting a packet now would deliver it ahead of
  // messages that are already known and already older.
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(16, 24), at_ms(80));
  hand_over(client, held.accepted);
  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 20).get(plan) == dfr::error::ok);
  REQUIRE(client.state() == rec::client_state::replaying);

  const auto refused = client.on_packet(kLineA, kSession, 24, 4, 0, at_ms(91));
  CHECK(refused.error_code() == dfr::error::invalid_argument);
}

TEST_CASE("finishing a replay that was never started is refused",
          "[recovery][client]") {
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 5), at_ms(0));
  CHECK(client.finish_replay().error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a stale snapshot is discarded and recovery continues",
          "[recovery][client]") {
  // Applying it would replace current state with older state. The right response is to
  // throw the snapshot away rather than the stream, so the client stays in recovery and
  // poll() asks again.
  auto client = abandoned_client();

  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 5).get(plan) == dfr::error::ok);
  CHECK(plan.verdict == rec::snapshot_verdict::stale);
  CHECK(client.state() == rec::client_state::recovering);
  CHECK(client.poll(at_ms(91)).what == rec::client_action::request_snapshot);
}

// ---------------------------------------------------------------------------
// The failures with no repair
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot behind the buffer fails the client",
          "[recovery][client]") {
  // The Glimpse race, end to end. There is nothing to salvage: the messages between the
  // snapshot and the buffer are in neither, and a client that carried on would publish a
  // plausible, permanently wrong book.
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(50, 58), at_ms(80));
  hand_over(client, held.accepted);
  REQUIRE(client.held().buffered() == range(50, 58));

  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 40).get(plan) == dfr::error::ok);
  CHECK(plan.verdict == rec::snapshot_verdict::behind_buffer);
  CHECK(plan.unfillable == range(40, 50));
  CHECK(client.state() == rec::client_state::failed);
}

TEST_CASE("a failed client keeps saying so", "[recovery][client]") {
  // A caller that polls once and ignores the answer must not then see idle and assume
  // everything is fine.
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(50, 58), at_ms(80));
  hand_over(client, held.accepted);
  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 40).get(plan) == dfr::error::ok);

  for (const std::int64_t t : {91, 200, 10'000}) {
    const auto verdict = client.poll(at_ms(t));
    CHECK(verdict.what == rec::client_action::restart);
    CHECK(verdict.reason == dfr::error::snapshot_behind_buffer);
  }
}

TEST_CASE("a failed client refuses further packets",
          "[recovery][client]") {
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(50, 58), at_ms(80));
  hand_over(client, held.accepted);
  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 40).get(plan) == dfr::error::ok);

  const auto refused = client.on_packet(kLineA, kSession, 58, 2, 0, at_ms(91));
  CHECK(refused.error_code() == dfr::error::snapshot_behind_buffer);
}

TEST_CASE("a snapshot from a new session is not judged against the old one's numbers",
          "[recovery][client]") {
  // The third defect the stateful fuzzer found, and the same shape as the first two: a component was told about
  // a session change and another one was not.
  //
  // on_snapshot() planned against `delivered_before()`, which belongs to the session the client is still on. A
  // snapshot for a *new* session renumbers from that session's beginning, so its sequence is small, and it was
  // compared against a large watermark from a stream that no longer exists, judged stale, and discarded. The
  // client would refuse the one thing that could recover it, and keep refusing.
  auto client = abandoned_client();
  REQUIRE(client.state() == rec::client_state::recovering);
  REQUIRE(client.delivered_before() == 16);

  // The feed restarts. The snapshot for the new session is at sequence 6, far below the old watermark.
  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession + 1, 6).get(plan) == dfr::error::ok);

  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.resume_from == 6);
  CHECK(client.delivered_before() == 6);
  // And nothing from the old session survives to be asked for.
  CHECK(client.total_missing() == 0);
  CHECK(client.retransmission().messages_outstanding() == 0);
}

TEST_CASE("a snapshot cancels the retransmit requests it supersedes",
          "[recovery][client]") {
  // Found by the stateful fuzzer under libFuzzer, after two hundred thousand rounds of the portable driver had
  // not reached it.
  //
  // on_snapshot() tells the gap tracker to abandon the holes below the snapshot point, and the tracker does not
  // speak to the requester. A hole that opens *while* the client is recovering adds a requester entry that the
  // snapshot then supersedes, and nothing removed it, so once the replay finished poll() went back to asking the
  // venue for messages the snapshot had already delivered.
  //
  // Not memory corruption, and not harmless either: a retransmit facility's retention window is the scarce
  // resource in a real recovery, and this spends it on ranges nobody needs while the requests that matter queue
  // behind them.
  auto client = abandoned_client();
  REQUIRE(client.state() == rec::client_state::recovering);

  // Packets keep arriving during recovery, and they can be gapped too. This opens 16..19 and buffers the rest.
  const auto held = offer(client, kLineA, range(20, 26), at_ms(80));
  hand_over(client, held.accepted);
  REQUIRE(client.retransmission().messages_outstanding() > 0);

  rec::snapshot_plan plan;
  REQUIRE(client.on_snapshot(kSession, 20).get(plan) == dfr::error::ok);
  REQUIRE(plan.verdict == rec::snapshot_verdict::usable);
  REQUIRE(client.finish_replay().has_value());

  // Everything below the resume point came from the snapshot, so nothing below it may be asked for again.
  CHECK(client.retransmission().messages_outstanding() == 0);
  for (int tick = 0; tick < 8; ++tick) {
    const auto decision = client.poll(at_ms(200 + tick * 50));
    if (decision.what == rec::client_action::send_retransmit_request) {
      CHECK(decision.range.first >= plan.resume_from);
    }
  }
}

TEST_CASE("a replay buffer that fills fails the client",
          "[recovery][client]") {
  // The recovery attempt cannot produce a correct book, and there is no way to paper over
  // it: a buffer that dropped its oldest entries instead would make the Glimpse check
  // above pass while the data was already gone.
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(16, 40), at_ms(80));
  REQUIRE(held.held_for_replay);

  dfr::error last = dfr::error::ok;
  for (std::uint64_t sequence = held.accepted.first;
       sequence < held.accepted.end && last == dfr::error::ok; ++sequence) {
    last = client.buffer_message(sequence, bytes_of("m")).error_code();
  }
  CHECK(last == dfr::error::recovery_buffer_overflow);
  CHECK(client.state() == rec::client_state::failed);
  CHECK(client.poll(at_ms(90)).reason == dfr::error::recovery_buffer_overflow);
}

TEST_CASE("a hole in the replay buffer fails the client",
          "[recovery][client]") {
  // Contiguity is enforced rather than tracked: a replay with a hole produces a wrong
  // book, so it is refused at the moment it becomes impossible instead of at the moment it
  // becomes visible.
  auto client = abandoned_client();
  const auto held = offer(client, kLineA, range(16, 20), at_ms(80));
  hand_over(client, held.accepted);

  const auto skipped = client.buffer_message(25, bytes_of("m"));
  CHECK(skipped.error_code() == dfr::error::sequence_gap);
  CHECK(client.state() == rec::client_state::failed);
}

TEST_CASE("lines carrying different content fails the client",
          "[recovery][client]") {
  // A/B redundancy rests on the lines being identical by construction. Once they are not,
  // neither copy can be trusted, and the client says so rather than preferring one.
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 5), at_ms(0), kSession, 0xDEAD);

  const auto diverged =
      client.on_packet(dfr_test::recovery::kLineB, kSession, 1, 4, 0xBEEF, at_ms(0));
  CHECK(diverged.error_code() == dfr::error::lines_diverged);
  CHECK(client.state() == rec::client_state::failed);
}

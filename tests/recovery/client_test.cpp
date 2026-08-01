// The client on the ordinary path: two lines in, one stream out, gaps chased.
//
// Escalation, snapshots and failure are in client_recovery_test.cpp.

#include <dfr/recovery/client.hpp>

#include "recovery/support/client_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace rec = dfr::recovery;

using dfr_test::recovery::at_ms;
using dfr_test::recovery::kLineA;
using dfr_test::recovery::kLineB;
using dfr_test::recovery::offer;
using dfr_test::recovery::range;
using dfr_test::recovery::readable_options;
using dfr_test::recovery::test_client;

// ---------------------------------------------------------------------------
// Getting started
// ---------------------------------------------------------------------------

TEST_CASE("a fresh client is synchronising and asks for nothing",
          "[recovery][client]") {
  test_client client{readable_options()};
  CHECK(client.state() == rec::client_state::synchronising);
  CHECK(client.poll(at_ms(0)).what == rec::client_action::idle);
  CHECK(client.delivered_through() == 0);
}

TEST_CASE("the first packet puts the client live", "[recovery][client]") {
  test_client client{readable_options()};
  const auto first = offer(client, kLineA, range(1'000, 1'005), at_ms(0));

  CHECK(client.state() == rec::client_state::live);
  CHECK(first.merge == rec::arbitration::deliver);
  CHECK(first.outcome == rec::sequencing::established);
  CHECK(first.accepted == range(1'000, 1'005));
  CHECK(first.delivered());
  CHECK(client.delivered_through() == 1'005);
  CHECK(client.total_missing() == 0);
}

TEST_CASE("a clean stream delivers everything and asks for nothing",
          "[recovery][client]") {
  test_client client{readable_options()};
  std::uint64_t sequence = 1;
  for (std::int64_t t = 0; t < 50; ++t) {
    const auto seen = offer(client, kLineA, range(sequence, sequence + 3), at_ms(t));
    CHECK(seen.delivered());
    CHECK(seen.gap_opened.empty());
    sequence += 3;
  }
  CHECK(client.total_missing() == 0);
  CHECK(client.poll(at_ms(100)).what == rec::client_action::idle);
  CHECK(client.retransmission().stats().requests_sent == 0);
}

// ---------------------------------------------------------------------------
// The two layers disagreeing usefully
// ---------------------------------------------------------------------------

TEST_CASE("a duplicate never reaches the sequence tracker",
          "[recovery][client]") {
  // The reason the arbiter runs first. Feeding both copies of every packet into sequence
  // tracking would report half the stream as regressed, and the duplicate count would
  // look like an error rate.
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 5), at_ms(0));
  const auto second = offer(client, kLineB, range(1, 5), at_ms(0));

  CHECK(second.merge == rec::arbitration::duplicate);
  CHECK(second.accepted.empty());
  CHECK_FALSE(second.delivered());
  CHECK(client.tracking().stats(rec::channel_id::at(0)).duplicates == 0);
  CHECK(client.arbitration().stats(kLineB).duplicates == 1);
}

TEST_CASE("a redundant pair delivers each message exactly once",
          "[recovery][client]") {
  test_client client{readable_options()};
  std::uint64_t sequence = 1;
  std::uint64_t delivered = 0;
  for (std::int64_t t = 0; t < 40; ++t) {
    const auto arrived = range(sequence, sequence + 2);
    for (const std::size_t line : {kLineA, kLineB}) {
      const auto seen = offer(client, line, arrived, at_ms(t));
      if (seen.delivered()) {
        delivered += seen.accepted.count();
      }
    }
    sequence += 2;
  }
  CHECK(delivered == 80);
  CHECK(client.delivered_through() == sequence);
  CHECK(client.total_missing() == 0);
}

// ---------------------------------------------------------------------------
// Gaps, and what arbitration saves
// ---------------------------------------------------------------------------

TEST_CASE("a gap is reported with the range a request needs",
          "[recovery][client]") {
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 11), at_ms(0));

  const auto jumped = offer(client, kLineA, range(20, 25), at_ms(1));
  CHECK(jumped.outcome == rec::sequencing::gap_opened);
  CHECK(jumped.gap_opened == range(11, 20));
  CHECK(jumped.delivered());  // live data keeps flowing past the hole
  CHECK(client.total_missing() == 9);
}

TEST_CASE("a gap on one line is covered by the other and never requested",
          "[recovery][client]") {
  // The property that justifies paying for a second line: the hole closes before the
  // settle delay expires, so no retransmit request is ever sent. A client that asked
  // immediately would generate recovery traffic the redundant line had already made
  // unnecessary.
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 6), at_ms(0));
  offer(client, kLineA, range(11, 16), at_ms(0));  // A lost 6..10
  REQUIRE(client.total_missing() == 5);

  const auto rescue = offer(client, kLineB, range(6, 11), at_ms(0));
  CHECK(rescue.outcome == rec::sequencing::gap_filled);
  CHECK(rescue.recovered == 5);
  CHECK(client.total_missing() == 0);

  CHECK(client.poll(at_ms(50)).what == rec::client_action::idle);
  CHECK(client.retransmission().stats().requests_sent == 0);
}

TEST_CASE("the slower line arriving first is simply the stream",
          "[recovery][client]") {
  // B leads for a while and A catches up. Neither line is preferred, so this must produce
  // no gaps at all rather than a gap on A followed by a fill.
  test_client client{readable_options()};
  offer(client, kLineB, range(1, 5), at_ms(0));
  offer(client, kLineB, range(5, 9), at_ms(1));
  offer(client, kLineA, range(1, 5), at_ms(2));
  offer(client, kLineA, range(5, 9), at_ms(2));
  offer(client, kLineA, range(9, 13), at_ms(3));

  CHECK(client.total_missing() == 0);
  CHECK(client.tracking().stats(rec::channel_id::at(0)).gaps_opened == 0);
  CHECK(client.delivered_through() == 13);
}

TEST_CASE("a gap neither line covers becomes a retransmit request",
          "[recovery][client]") {
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 6), at_ms(0));
  offer(client, kLineB, range(1, 6), at_ms(0));
  // Both lines miss 6..10.
  offer(client, kLineA, range(11, 16), at_ms(0));
  offer(client, kLineB, range(11, 16), at_ms(0));

  CHECK(client.poll(at_ms(0)).what == rec::client_action::idle);  // still settling

  const auto asked = client.poll(at_ms(1));
  CHECK(asked.what == rec::client_action::send_retransmit_request);
  CHECK(asked.range == range(6, 11));
  CHECK(asked.attempt == 1);
}

TEST_CASE("a retransmit arriving closes the request",
          "[recovery][client]") {
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 6), at_ms(0));
  offer(client, kLineA, range(11, 16), at_ms(0));
  REQUIRE(client.poll(at_ms(1)).what == rec::client_action::send_retransmit_request);

  const auto repair = offer(client, kLineA, range(6, 11), at_ms(2));
  CHECK(repair.outcome == rec::sequencing::gap_filled);
  CHECK(client.total_missing() == 0);
  CHECK(client.poll(at_ms(11)).what == rec::client_action::idle);
  CHECK(client.retransmission().stats().requests_sent == 1);
}

TEST_CASE("a partial retransmit leaves the rest being asked for",
          "[recovery][client]") {
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 6), at_ms(0));
  offer(client, kLineA, range(21, 26), at_ms(0));  // 6..20 missing
  REQUIRE(client.poll(at_ms(1)).range == range(6, 21));

  offer(client, kLineA, range(6, 11), at_ms(2));
  CHECK(client.total_missing() == 10);
  CHECK(client.poll(at_ms(11)).range == range(11, 21));
}

// ---------------------------------------------------------------------------
// Liveness and sessions
// ---------------------------------------------------------------------------

TEST_CASE("a silent line is reported while the stream stays perfect",
          "[recovery][client]") {
  // The alarm worth having: losing one line of a pair is invisible in the data, so unless
  // the client says so, the first anyone hears is when the second line fails too.
  test_client client{readable_options()};
  std::uint64_t sequence = 1;
  for (std::int64_t t = 0; t < 20; ++t) {
    offer(client, kLineA, range(sequence, sequence + 2), at_ms(t));
    offer(client, kLineB, range(sequence, sequence + 2), at_ms(t));
    sequence += 2;
  }
  REQUIRE(client.live_lines(at_ms(20)) == 2);

  for (std::int64_t t = 20; t < 200; ++t) {
    offer(client, kLineA, range(sequence, sequence + 2), at_ms(t));
    sequence += 2;
  }
  CHECK(client.live_lines(at_ms(199)) == 1);
  CHECK(client.total_missing() == 0);  // and nothing was lost
}

TEST_CASE("a session change re-synchronises rather than reporting a gap",
          "[recovery][client][regression]") {
  // The new session renumbers from its own beginning. Carrying the old watermark across
  // would classify every packet of the new session as a duplicate, and the client would
  // sit silent forever, which is why the session check runs before arbitration rather
  // than after it.
  test_client client{readable_options()};
  offer(client, kLineA, range(1'000, 1'010), at_ms(0));
  offer(client, kLineA, range(1'020, 1'030), at_ms(0));  // a hole, being chased
  REQUIRE(client.total_missing() == 10);

  const auto renumbered = offer(client, kLineA, range(1, 5), at_ms(1), 0x9999);
  CHECK(renumbered.merge == rec::arbitration::deliver);
  // session_reset rather than established: the caller needs to know its book belongs to a
  // stream that no longer exists, which is more than "this is the first packet".
  CHECK(renumbered.outcome == rec::sequencing::session_reset);
  CHECK(renumbered.accepted == range(1, 5));
  CHECK(client.state() == rec::client_state::live);
  CHECK(client.delivered_through() == 5);

  // And the old session's hole is not chased into the new one, where those sequence
  // numbers mean something else entirely.
  CHECK(client.total_missing() == 0);
  CHECK(client.poll(at_ms(50)).what == rec::client_action::idle);
}

// ---------------------------------------------------------------------------
// Heartbeats
// ---------------------------------------------------------------------------

TEST_CASE("a heartbeat announcing a jump opens a gap",
          "[recovery][client][regression]") {
  // On IEX two thirds of all packets are heartbeats, and a heartbeat carries the sequence
  // number of the *next* message, so a heartbeat whose number has jumped is how a receiver
  // learns it missed the end of a quiet period. The client used to return early on any
  // packet carrying no messages, which meant that jump was never noticed at all.
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 11), at_ms(0));
  REQUIRE(client.total_missing() == 0);

  const auto beat = offer(client, kLineA, range(11, 11), at_ms(1));  // in position
  CHECK(beat.outcome == rec::sequencing::in_order);
  CHECK(client.total_missing() == 0);

  const auto jumped = offer(client, kLineA, range(21, 21), at_ms(2));
  CHECK(jumped.outcome == rec::sequencing::gap_opened);
  CHECK(jumped.gap_opened == range(11, 21));
  CHECK(client.total_missing() == 10);
}

TEST_CASE("a retransmit for a heartbeat-announced gap is delivered once",
          "[recovery][client][regression]") {
  // The defect this pins was found by pointing the verify tool at a real capture, and it
  // could not happen without heartbeats. A heartbeat advances the tracker's expectation
  // while delivering nothing, so it cannot advance the arbiter's watermark; the two then
  // disagreed, the hole sat *above* the watermark, and the retransmit that filled it counted
  // as both newly arrived and newly repaired. One message delivered twice corrupts a book
  // exactly as thoroughly as losing one.
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 11), at_ms(0));
  offer(client, kLineA, range(21, 21), at_ms(1));  // heartbeat announcing a jump
  REQUIRE(client.total_missing() == 10);

  const auto repair = offer(client, kLineA, range(11, 21), at_ms(2));
  CHECK(repair.outcome == rec::sequencing::gap_filled);
  CHECK(repair.recovered == 10);
  CHECK(client.total_missing() == 0);

  // Counted once, not twice: the repaired range must not also appear as newly accepted.
  CHECK(repair.accepted.empty());
  std::uint64_t once = 0;
  for (const auto& repaired : client.last_recovered().ranges()) {
    once += repaired.count();
  }
  CHECK(once == 10);
}

TEST_CASE("a heartbeat behind the stream is a duplicate, not a gap",
          "[recovery][client]") {
  // A delayed or duplicated heartbeat carries an old sequence number. It must not rewind
  // anything, and it must not be reported as a regression worth acting on.
  test_client client{readable_options()};
  offer(client, kLineA, range(1, 11), at_ms(0));

  const auto late = offer(client, kLineA, range(5, 5), at_ms(1));
  CHECK(late.recovered == 0);
  CHECK(client.total_missing() == 0);
  CHECK(client.delivered_through() == 11);
}

TEST_CASE("every client state and action has a distinct name",
          "[recovery][client]") {
  CHECK(rec::name_of(rec::client_state::synchronising) == "synchronising");
  CHECK(rec::name_of(rec::client_state::live) == "live");
  CHECK(rec::name_of(rec::client_state::recovering) == "recovering");
  CHECK(rec::name_of(rec::client_state::replaying) == "replaying");
  CHECK(rec::name_of(rec::client_state::failed) == "failed");
  CHECK(rec::name_of(rec::client_action::idle) == "idle");
  CHECK(rec::name_of(rec::client_action::request_snapshot) == "request_snapshot");
  CHECK(rec::name_of(rec::client_action::restart) == "restart");
}

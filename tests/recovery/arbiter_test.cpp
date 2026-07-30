// Merging two lines into one stream: first copy wins, exactly once.
//
// Line health — liveness, how far behind, divergence — is in arbiter_health_test.cpp.
// This file is only about the merge.

#include <dfr/recovery/arbiter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace rec = dfr::recovery;

namespace {

using test_arbiter = rec::arbiter<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

constexpr std::size_t kLineA = 0;
constexpr std::size_t kLineB = 1;

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

// Offers a packet and requires it to be accepted. A failure here is divergence, which
// every test in this file treats as a bug in the test.
rec::arbitration_result offer(test_arbiter& arbiter, std::size_t line,
                              rec::sequence_range arrived,
                              std::uint64_t digest = 0) {
  rec::arbitration_result out;
  REQUIRE(arbiter.offer(line, arrived, digest, test_time{}).get(out) ==
          dfr::error::ok);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// One line at a time
// ---------------------------------------------------------------------------

TEST_CASE("a fresh arbiter has delivered nothing", "[recovery][arbiter]") {
  const test_arbiter arbiter;
  CHECK(arbiter.delivered_through() == 0);
  CHECK(arbiter.stats(kLineA).packets == 0);
}

TEST_CASE("the first packet on a line is delivered whole",
          "[recovery][arbiter]") {
  // Including when the feed is joined mid-session at a high sequence number: the
  // watermark starts at zero meaning "nothing delivered", not "everything below is
  // known".
  test_arbiter arbiter;
  const auto first = offer(arbiter, kLineA, range(1'000, 1'005));

  CHECK(first.outcome == rec::arbitration::deliver);
  CHECK(first.deliver == range(1'000, 1'005));
  CHECK(first.won());
  CHECK(arbiter.delivered_through() == 1'005);
  CHECK(arbiter.stats(kLineA).first_copies == 1);
}

TEST_CASE("a repeat on the same line is a duplicate", "[recovery][arbiter]") {
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(10, 20));

  const auto again = offer(arbiter, kLineA, range(10, 20));
  CHECK(again.outcome == rec::arbitration::duplicate);
  CHECK(again.deliver.empty());
  CHECK_FALSE(again.won());
  CHECK(arbiter.delivered_through() == 20);
  CHECK(arbiter.stats(kLineA).duplicates == 1);
}

TEST_CASE("a heartbeat is neither a first copy nor a duplicate",
          "[recovery][arbiter]") {
  // It carries no messages, so it cannot win or lose a race. It still counts as a sign
  // of life, which is the reason the liveness bookkeeping happens before the emptiness
  // check rather than after it.
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(10, 20));

  const auto beat = offer(arbiter, kLineA, range(20, 20));
  CHECK(beat.outcome == rec::arbitration::duplicate);
  CHECK_FALSE(beat.won());
  CHECK(arbiter.stats(kLineA).packets == 2);
  CHECK(arbiter.stats(kLineA).first_copies == 1);   // unchanged
  CHECK(arbiter.stats(kLineA).duplicates == 0);     // and not counted as one either
  CHECK(arbiter.is_live(kLineA, test_time{}));
}

// ---------------------------------------------------------------------------
// Two lines
// ---------------------------------------------------------------------------

TEST_CASE("the second copy of a packet is discarded",
          "[recovery][arbiter]") {
  test_arbiter arbiter;
  const auto from_a = offer(arbiter, kLineA, range(10, 20));
  const auto from_b = offer(arbiter, kLineB, range(10, 20));

  CHECK(from_a.outcome == rec::arbitration::deliver);
  CHECK(from_b.outcome == rec::arbitration::duplicate);
  CHECK(arbiter.stats(kLineA).first_copies == 1);
  CHECK(arbiter.stats(kLineB).first_copies == 0);
  CHECK(arbiter.stats(kLineB).duplicates == 1);
}

TEST_CASE("a healthy pair delivers every message exactly once",
          "[recovery][arbiter]") {
  // The property the whole arrangement exists for, stated as an equality rather than a
  // description. Both lines carry everything; the merged stream is each message once.
  test_arbiter arbiter;
  std::vector<rec::sequence_range> delivered;
  std::uint64_t sequence = 1;

  for (int i = 0; i < 50; ++i) {
    const auto arrived = range(sequence, sequence + 4);
    for (const std::size_t line : {kLineA, kLineB}) {
      const auto verdict = offer(arbiter, line, arrived);
      if (verdict.won()) {
        delivered.push_back(verdict.deliver);
      }
    }
    sequence += 4;
  }

  REQUIRE(delivered.size() == 50);
  CHECK(delivered.front().first == 1);
  CHECK(delivered.back().end == sequence);
  for (std::size_t i = 1; i < delivered.size(); ++i) {
    CHECK(delivered[i].first == delivered[i - 1].end);  // no gap, no overlap
  }
  CHECK(arbiter.delivered_through() == sequence);
  CHECK(arbiter.stats(kLineA).first_copies == 50);
  CHECK(arbiter.stats(kLineB).duplicates == 50);
}

TEST_CASE("whichever line arrives first wins, packet by packet",
          "[recovery][arbiter]") {
  // The realistic case: the lines take turns, because path latency is not a constant.
  // Neither line is preferred and neither is trusted more.
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(1, 5));
  offer(arbiter, kLineB, range(1, 5));
  offer(arbiter, kLineB, range(5, 9));
  offer(arbiter, kLineA, range(5, 9));
  offer(arbiter, kLineA, range(9, 13));
  offer(arbiter, kLineB, range(9, 13));

  CHECK(arbiter.stats(kLineA).first_copies == 2);
  CHECK(arbiter.stats(kLineB).first_copies == 1);
  CHECK(arbiter.delivered_through() == 13);
}

TEST_CASE("a gap on one line is covered by the other with nothing lost",
          "[recovery][arbiter]") {
  // Line A drops a burst; line B carries it. The merged stream is intact, which is the
  // entire point of paying for a second line.
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(1, 5));
  // A misses 5..8 entirely.
  const auto rescue = offer(arbiter, kLineB, range(5, 9));
  offer(arbiter, kLineA, range(9, 13));

  CHECK(rescue.outcome == rec::arbitration::deliver);
  CHECK(rescue.deliver == range(5, 9));
  CHECK(arbiter.delivered_through() == 13);
  CHECK(arbiter.stats(kLineB).first_copies == 1);
}

// ---------------------------------------------------------------------------
// Partial overlap
// ---------------------------------------------------------------------------

TEST_CASE("a packet straddling the watermark delivers only its tail",
          "[recovery][arbiter]") {
  // Happens when the two lines frame the same messages into differently sized packets,
  // which some venues permit. Delivering the whole packet would duplicate messages
  // downstream; discarding it would lose the tail.
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(10, 20));

  const auto straddle = offer(arbiter, kLineB, range(15, 25));
  CHECK(straddle.outcome == rec::arbitration::partial);
  CHECK(straddle.deliver == range(20, 25));
  CHECK(arbiter.delivered_through() == 25);
  CHECK(arbiter.stats(kLineB).first_copies == 1);
}

TEST_CASE("a packet entirely below the watermark contributes nothing",
          "[recovery][arbiter]") {
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(10, 30));

  const auto late = offer(arbiter, kLineB, range(12, 18));
  CHECK(late.outcome == rec::arbitration::duplicate);
  CHECK(late.deliver.empty());
  CHECK(arbiter.delivered_through() == 30);
}

TEST_CASE("the watermark never moves backwards", "[recovery][arbiter]") {
  // A late packet must not rewind the merged stream, or messages already handed
  // downstream would be delivered a second time when the next copy arrived.
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(100, 200));
  offer(arbiter, kLineB, range(10, 20));
  offer(arbiter, kLineB, range(50, 60));

  CHECK(arbiter.delivered_through() == 200);
}

TEST_CASE("a jump forward is delivered rather than held",
          "[recovery][arbiter]") {
  // The arbiter deliberately does not know what a gap is: it makes each message cross
  // the boundary once and leaves "what is still missing" to gap_tracker, which already
  // answers that question correctly. Two answers to one question is worse than one.
  test_arbiter arbiter;
  offer(arbiter, kLineA, range(1, 5));

  const auto jumped = offer(arbiter, kLineA, range(500, 505));
  CHECK(jumped.outcome == rec::arbitration::deliver);
  CHECK(jumped.deliver == range(500, 505));
  CHECK(arbiter.delivered_through() == 505);
}

TEST_CASE("every arbitration outcome has a distinct name",
          "[recovery][arbiter]") {
  CHECK(rec::name_of(rec::arbitration::deliver) == "deliver");
  CHECK(rec::name_of(rec::arbitration::duplicate) == "duplicate");
  CHECK(rec::name_of(rec::arbitration::partial) == "partial");
}

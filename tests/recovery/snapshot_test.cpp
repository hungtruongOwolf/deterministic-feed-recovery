// Classifying a snapshot, including the race that silently produces a wrong book.
//
// The function under test is pure and constexpr, so most of this file could be
// static_asserts. It is written as runtime checks anyway, because when one of these fails
// the expansion Catch2 prints is the fastest route to understanding *which* case was
// misclassified, and the last test pins the constexpr-ness separately.

#include <dfr/recovery/snapshot.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace rec = dfr::recovery;

namespace {

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

constexpr rec::sequence_range nothing_buffered() {
  return rec::sequence_range{};
}

}  // namespace

// ---------------------------------------------------------------------------
// The race
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot older than the buffer leaves a permanent hole",
          "[recovery][snapshot]") {
  // The failure the library exists to catch. The client buffered from 10; the snapshot
  // reflects state through 4. Messages 5 through 9 are in neither, and no retransmit can
  // help because a client that does not check does not know it has a gap.
  const auto plan = rec::plan_snapshot(5, range(10, 20));

  CHECK(plan.verdict == rec::snapshot_verdict::behind_buffer);
  CHECK(plan.unfillable == range(5, 10));
  CHECK(plan.reason() == dfr::error::snapshot_behind_buffer);
  CHECK(dfr::is_fatal(plan.reason()));

  // Nothing is salvaged, deliberately. Replaying the buffer on top of the snapshot is
  // exactly the mistake: it produces a book that is plausible, internally consistent and
  // permanently missing five messages.
  CHECK(plan.replay.empty());
  CHECK(plan.discard.empty());
}

TEST_CASE("the size of the permanent hole is reported exactly",
          "[recovery][snapshot]") {
  // It is the number an operator needs: how much data this client would have been
  // silently missing for the rest of the session.
  const auto plan = rec::plan_snapshot(1'000, range(5'000, 5'001));
  CHECK(plan.unfillable == range(1'000, 5'000));
  CHECK(plan.unfillable.count() == 4'000);
}

TEST_CASE("a snapshot one message short of the buffer is still behind it",
          "[recovery][snapshot][regression]") {
  // The boundary that decides whether the check is worth anything. The snapshot
  // establishes state through 8; the buffer starts at 10; message 9 is missing. An
  // inclusive-versus-exclusive slip here would classify this as usable and lose exactly
  // one message: the hardest possible amount to notice.
  const auto plan = rec::plan_snapshot(9, range(10, 20));
  CHECK(plan.verdict == rec::snapshot_verdict::behind_buffer);
  CHECK(plan.unfillable == range(9, 10));
  CHECK(plan.unfillable.count() == 1);
}

TEST_CASE("a snapshot meeting the buffer exactly is usable",
          "[recovery][snapshot]") {
  // The other side of that boundary. The snapshot establishes state through 9 and the
  // buffer starts at 10, so they meet with nothing between them and the whole buffer is
  // replayed.
  const auto plan = rec::plan_snapshot(10, range(10, 20));
  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.unfillable.empty());
  CHECK(plan.discard.empty());
  CHECK(plan.replay == range(10, 20));
  CHECK(plan.resume_from == 20);
}

// ---------------------------------------------------------------------------
// The ordinary outcome
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot inside the buffer splits it into discard and replay",
          "[recovery][snapshot]") {
  // The normal case: the snapshot lands part-way through what the client buffered, so the
  // front of the buffer is redundant and the back is the repair.
  const auto plan = rec::plan_snapshot(15, range(10, 20));

  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.discard == range(10, 15));
  CHECK(plan.replay == range(15, 20));
  CHECK(plan.resume_from == 20);
  CHECK(plan.reason() == dfr::error::ok);
}

TEST_CASE("discard and replay together account for the whole buffer",
          "[recovery][snapshot]") {
  // Both ranges are returned even though one is derivable from the other, because the
  // derivation is where the off-by-one lives: a caller computing it will eventually
  // replay a message twice or skip one, and either makes a wrong book from correct
  // inputs.
  for (std::uint64_t at = 10; at <= 20; ++at) {
    const auto plan = rec::plan_snapshot(at, range(10, 20));
    REQUIRE(plan.verdict == rec::snapshot_verdict::usable);
    CHECK(plan.discard.count() + plan.replay.count() == 10);
    if (!plan.discard.empty() && !plan.replay.empty()) {
      CHECK(plan.discard.end == plan.replay.first);  // no overlap, no hole
    }
  }
}

TEST_CASE("a snapshot ahead of the buffer makes the buffer redundant",
          "[recovery][snapshot]") {
  // The harmless direction, and it must not be confused with the dangerous one. The
  // snapshot covers everything buffered and more, so the buffer is discarded whole and
  // nothing is replayed.
  const auto plan = rec::plan_snapshot(50, range(10, 20));

  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.discard == range(10, 20));
  CHECK(plan.replay.empty());
  CHECK(plan.unfillable.empty());
  CHECK(plan.resume_from == 50);
}

TEST_CASE("a snapshot at the buffer's end discards all of it",
          "[recovery][snapshot]") {
  const auto plan = rec::plan_snapshot(20, range(10, 20));
  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.discard == range(10, 20));
  CHECK(plan.replay.empty());
  CHECK(plan.resume_from == 20);
}

TEST_CASE("an empty buffer is not itself a problem", "[recovery][snapshot]") {
  // A client that has buffered nothing yet can still apply a snapshot and continue from
  // it. Whether it *started* buffering in time is a fact this function cannot see, which
  // is why the comment on the case says so rather than letting `usable` imply it.
  const auto plan = rec::plan_snapshot(100, nothing_buffered());
  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.discard.empty());
  CHECK(plan.replay.empty());
  CHECK(plan.resume_from == 100);
}

// ---------------------------------------------------------------------------
// Staleness
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot the stream has already passed is stale",
          "[recovery][snapshot]") {
  // Applying it would replace current state with older state. Harmless, and the right
  // response is to throw the snapshot away rather than the stream, which is why it is
  // not fatal.
  const auto plan = rec::plan_snapshot(50, range(100, 110), 200);

  CHECK(plan.verdict == rec::snapshot_verdict::stale);
  CHECK(plan.reason() == dfr::error::snapshot_stale);
  CHECK_FALSE(dfr::is_fatal(plan.reason()));
  CHECK(plan.resume_from == 200);
  CHECK(plan.unfillable.empty());
}

TEST_CASE("staleness is checked before the buffer is considered",
          "[recovery][snapshot]") {
  // If the stream has already progressed past the snapshot, what the buffer holds is
  // beside the point, and reporting behind_buffer here would escalate a harmless
  // discard into a fatal restart.
  const auto plan = rec::plan_snapshot(50, range(100, 110), 60);
  CHECK(plan.verdict == rec::snapshot_verdict::stale);
}

TEST_CASE("a snapshot exactly at the delivered watermark is stale",
          "[recovery][snapshot]") {
  // It establishes state through watermark - 1, all of which has already gone
  // downstream, so it adds nothing.
  const auto plan = rec::plan_snapshot(200, nothing_buffered(), 200);
  CHECK(plan.verdict == rec::snapshot_verdict::stale);
}

TEST_CASE("a snapshot one past the watermark is useful",
          "[recovery][snapshot]") {
  const auto plan = rec::plan_snapshot(201, nothing_buffered(), 200);
  CHECK(plan.verdict == rec::snapshot_verdict::usable);
  CHECK(plan.resume_from == 201);
}

TEST_CASE("a cold start is never stale", "[recovery][snapshot]") {
  // already_delivered defaults to zero, and any real snapshot sequence is above it.
  CHECK(rec::plan_snapshot(1, nothing_buffered()).verdict ==
        rec::snapshot_verdict::usable);
}

// ---------------------------------------------------------------------------
// Totality
// ---------------------------------------------------------------------------

TEST_CASE("every snapshot position is classified, and only one way",
          "[recovery][snapshot]") {
  // A sweep across every relationship the three inputs can have. The point is not the
  // individual answers(those are pinned above) but that no combination falls through
  // unclassified or is classified as both dangerous and fine.
  const auto buffered = range(100, 110);
  for (std::uint64_t at = 1; at <= 200; ++at) {
    for (const std::uint64_t delivered : {std::uint64_t{0}, std::uint64_t{50}}) {
      const auto plan = rec::plan_snapshot(at, buffered, delivered);

      const bool dangerous = plan.verdict == rec::snapshot_verdict::behind_buffer;
      CHECK(dangerous == !plan.unfillable.empty());
      if (dangerous) {
        CHECK(plan.replay.empty());
        CHECK(plan.discard.empty());
      }
      if (plan.verdict == rec::snapshot_verdict::usable) {
        CHECK(plan.resume_from >= at);
      }
    }
  }
}

TEST_CASE("every verdict has a distinct name", "[recovery][snapshot]") {
  CHECK(rec::name_of(rec::snapshot_verdict::usable) == "usable");
  CHECK(rec::name_of(rec::snapshot_verdict::behind_buffer) == "behind_buffer");
  CHECK(rec::name_of(rec::snapshot_verdict::stale) == "stale");
}

TEST_CASE("the whole classification is available at compile time",
          "[recovery][snapshot]") {
  // Which means the boundary cases are checked by the build, not only by a test run.
  static_assert(rec::plan_snapshot(9, range(10, 20)).verdict ==
                rec::snapshot_verdict::behind_buffer);
  static_assert(rec::plan_snapshot(10, range(10, 20)).verdict ==
                rec::snapshot_verdict::usable);
  static_assert(rec::plan_snapshot(15, range(10, 20)).replay == range(15, 20));
  static_assert(rec::plan_snapshot(50, range(100, 110), 200).verdict ==
                rec::snapshot_verdict::stale);
  SUCCEED("the static_asserts above are the test");
}

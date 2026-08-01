// Line health: which line is alive, how far behind it is, and whether the two agree.
//
// Operationally this matters more than the merge. Losing one line of a redundant pair
// is invisible in the data(the stream stays perfect) so unless the receiver reports
// it, the first anyone hears is when the second line fails too.

#include <dfr/recovery/arbiter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace rec = dfr::recovery;

namespace {

using test_arbiter = rec::arbiter<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

constexpr std::size_t kLineA = 0;
constexpr std::size_t kLineB = 1;

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

test_time at_ms(std::int64_t millis) {
  return test_time{} + std::chrono::milliseconds{millis};
}

rec::arbiter_options readable_options() {
  rec::arbiter_options options;
  options.liveness_timeout = std::chrono::milliseconds{100};
  REQUIRE(options.validate().has_value());
  return options;
}

rec::arbitration_result offer(test_arbiter& arbiter, std::size_t line,
                              rec::sequence_range arrived, test_time now,
                              std::uint64_t digest = 0) {
  rec::arbitration_result out;
  REQUIRE(arbiter.offer(line, arrived, digest, now).get(out) == dfr::error::ok);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

TEST_CASE("the default arbiter options are legal", "[recovery][arbiter]") {
  CHECK(rec::arbiter_options{}.validate().has_value());
}

TEST_CASE("a non-positive liveness timeout is rejected",
          "[recovery][arbiter]") {
  // Zero would mark every line down the instant after its last packet, so a healthy
  // pair would alarm continuously and the alarm would stop meaning anything.
  rec::arbiter_options options;
  options.liveness_timeout = dfr::duration::zero();
  CHECK(options.validate().error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------

TEST_CASE("a line never heard from is not live", "[recovery][arbiter]") {
  // The answer that makes a start-up check work: a receiver configured for two lines and
  // wired to one should report the second as down immediately, not after the first
  // timeout has elapsed.
  const test_arbiter arbiter{readable_options()};
  CHECK_FALSE(arbiter.is_live(kLineA, at_ms(0)));
  CHECK_FALSE(arbiter.is_live(kLineB, at_ms(0)));
  CHECK(arbiter.live_lines(2, at_ms(0)) == 0);
}

TEST_CASE("a line is live until its timeout elapses", "[recovery][arbiter]") {
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(1, 5), at_ms(0));

  CHECK(arbiter.is_live(kLineA, at_ms(0)));
  CHECK(arbiter.is_live(kLineA, at_ms(99)));
  CHECK_FALSE(arbiter.is_live(kLineA, at_ms(100)));
}

TEST_CASE("liveness is measured from the last packet seen, not the last one won",
          "[recovery][arbiter][regression]") {
  // A line that consistently loses every race by a microsecond is perfectly healthy and
  // is the entire reason the second line exists. Measuring from the last *win* would
  // report the slower line as down and hide the one failure that matters.
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(1, 5), at_ms(0));

  for (std::int64_t t = 1; t <= 300; ++t) {
    offer(arbiter, kLineA, range(1, 5), at_ms(t));   // A keeps winning
    offer(arbiter, kLineB, range(1, 5), at_ms(t));   // B never wins anything
  }

  CHECK(arbiter.stats(kLineB).first_copies == 0);
  CHECK(arbiter.is_live(kLineB, at_ms(300)));
  CHECK(arbiter.live_lines(2, at_ms(300)) == 2);
}

TEST_CASE("a silent line is reported down while the other keeps running",
          "[recovery][arbiter]") {
  // The alarm worth having. The data is still perfect, which is exactly why nothing
  // else will notice.
  test_arbiter arbiter{readable_options()};
  std::uint64_t sequence = 1;
  for (std::int64_t t = 0; t < 50; ++t) {
    offer(arbiter, kLineA, range(sequence, sequence + 2), at_ms(t));
    offer(arbiter, kLineB, range(sequence, sequence + 2), at_ms(t));
    sequence += 2;
  }
  // B stops here; A carries on.
  for (std::int64_t t = 50; t < 200; ++t) {
    offer(arbiter, kLineA, range(sequence, sequence + 2), at_ms(t));
    sequence += 2;
  }

  CHECK(arbiter.is_live(kLineA, at_ms(199)));
  CHECK_FALSE(arbiter.is_live(kLineB, at_ms(199)));
  CHECK(arbiter.live_lines(2, at_ms(199)) == 1);
}

// ---------------------------------------------------------------------------
// How far behind
// ---------------------------------------------------------------------------

TEST_CASE("the leading line is not behind", "[recovery][arbiter]") {
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(1, 100), at_ms(0));
  CHECK(arbiter.messages_behind(kLineA) == 0);
}

TEST_CASE("a lagging line reports how far back it is",
          "[recovery][arbiter]") {
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(1, 100), at_ms(0));
  offer(arbiter, kLineB, range(1, 60), at_ms(0));

  CHECK(arbiter.messages_behind(kLineB) == 40);
}

TEST_CASE("a line that was never seen is behind by the whole stream",
          "[recovery][arbiter]") {
  // Not zero. A silent line has delivered nothing, and reporting it as level with the
  // leader is the kind of comforting number that lets a dead line go unnoticed.
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(1, 1'000), at_ms(0));
  CHECK(arbiter.messages_behind(kLineB) == 1'000);
}

TEST_CASE("catching up brings a line level", "[recovery][arbiter]") {
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(1, 100), at_ms(0));
  REQUIRE(arbiter.messages_behind(kLineB) == 100);

  offer(arbiter, kLineB, range(1, 100), at_ms(1));
  CHECK(arbiter.messages_behind(kLineB) == 0);
}

// ---------------------------------------------------------------------------
// Divergence
// ---------------------------------------------------------------------------

TEST_CASE("two lines carrying the same bytes agree", "[recovery][arbiter]") {
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(10, 20), at_ms(0), 0xDEAD);
  offer(arbiter, kLineB, range(10, 20), at_ms(0), 0xDEAD);
  SUCCEED("no divergence reported");
}

TEST_CASE("two lines disagreeing on content is fatal, not a vote",
          "[recovery][arbiter]") {
  // The arbiter never prefers a line. A/B redundancy rests on the lines being identical
  // by construction; once they are not, neither copy can be trusted, and silently
  // taking whichever arrived first is how a receiver ends up confidently wrong.
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(10, 20), at_ms(0), 0xDEAD);

  const auto diverged = arbiter.offer(kLineB, range(10, 20), 0xBEEF, at_ms(0));
  CHECK_FALSE(diverged.has_value());
  CHECK(diverged.error_code() == dfr::error::lines_diverged);
  CHECK(dfr::is_fatal(diverged.error_code()));
}

TEST_CASE("a differently framed range is not a divergence",
          "[recovery][arbiter]") {
  // Some venues let the two lines split the same messages into different packets, so a
  // digest computed over a different range is legitimately different. Comparing on
  // overlap alone would make that a false alarm, and a false alarm on a fatal error is
  // worse than a missed one, because the fix is to switch the check off.
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(10, 20), at_ms(0), 0xDEAD);
  offer(arbiter, kLineB, range(10, 15), at_ms(0), 0x1111);
  offer(arbiter, kLineB, range(15, 20), at_ms(0), 0x2222);
  SUCCEED("different framing is not disagreement");
}

TEST_CASE("divergence checking can be switched off", "[recovery][arbiter]") {
  // For a caller with no cheap digest to hand. Off is honest; passing an arbitrary
  // number would produce false positives on the one error that is fatal.
  rec::arbiter_options options = readable_options();
  options.detect_divergence = false;
  test_arbiter arbiter{options};

  offer(arbiter, kLineA, range(10, 20), at_ms(0), 0xDEAD);
  const auto ignored = arbiter.offer(kLineB, range(10, 20), 0xBEEF, at_ms(0));
  CHECK(ignored.has_value());
}

TEST_CASE("divergence is only detected within the remembered history",
          "[recovery][arbiter]") {
  // The limit is real and is asserted rather than left to be discovered: a line running
  // more than kDigestHistory packets behind is checked for duplication but no longer for
  // disagreement. A check that quietly stops checking would be worse than none.
  test_arbiter arbiter{readable_options()};
  offer(arbiter, kLineA, range(0, 1), at_ms(0), 0xDEAD);
  for (std::uint64_t i = 1; i <= rec::kDigestHistory; ++i) {
    offer(arbiter, kLineA, range(i, i + 1), at_ms(0), i);
  }

  // The first range has aged out of the ring, so its digest is no longer compared.
  const auto unnoticed = arbiter.offer(kLineB, range(0, 1), 0xBEEF, at_ms(0));
  CHECK(unnoticed.has_value());
}

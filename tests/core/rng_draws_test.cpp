#include <dfr/core/rng.hpp>
#include <dfr/core/detail/wide_multiply.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <set>
#include <vector>

namespace {

std::vector<std::uint64_t> take(dfr::prng& rng, std::size_t count) {
  std::vector<std::uint64_t> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(rng.next());
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Probabilities
// ---------------------------------------------------------------------------

TEST_CASE("chance() respects its ratio", "[core][rng]") {
  dfr::prng rng{808};
  constexpr int kTrials = 20'000;

  int hits = 0;
  for (int i = 0; i < kTrials; ++i) {
    if (rng.chance(dfr::percent(25))) {
      ++hits;
    }
  }
  CHECK(hits > kTrials * 22 / 100);
  CHECK(hits < kTrials * 28 / 100);
}

TEST_CASE("kNever and kAlways are free and total", "[core][rng]") {
  dfr::prng rng{4};
  const auto before = rng.save();

  for (int i = 0; i < 100; ++i) {
    CHECK_FALSE(rng.chance(dfr::kNever));
    CHECK(rng.chance(dfr::kAlways));
  }

  // Consuming nothing is the load-bearing property. A fault site configured off
  // must not perturb the stream, otherwise disabling one site during shrinking
  // invalidates every draw after it and the reduced case stops reproducing.
  CHECK(rng.save() == before);
}

TEST_CASE("a ratio expresses probabilities below one per cent",
          "[core][rng]") {
  // FoundationDB's BUGGIFY sites fire at rates that matter down to fractions of
  // a per cent, which a percentage-based API cannot express at all.
  dfr::prng rng{2718};
  constexpr int kTrials = 200'000;

  int hits = 0;
  for (int i = 0; i < kTrials; ++i) {
    if (rng.chance(dfr::ratio{1, 1000})) {
      ++hits;
    }
  }
  CHECK(hits > 130);
  CHECK(hits < 270);
}

TEST_CASE("an impossible probability is a programmer error", "[core][rng]") {
  DFR_CHECK_ABORTS({
    dfr::prng rng{1};
    static_cast<void>(rng.chance(dfr::ratio{3, 2}));
  });
  DFR_CHECK_ABORTS({
    dfr::prng rng{1};
    static_cast<void>(rng.chance(dfr::ratio{1, 0}));
  });
  DFR_CHECK_ABORTS({
    dfr::prng rng{1};
    static_cast<void>(rng.below(0));
  });
  DFR_CHECK_ABORTS({
    dfr::prng rng{1};
    static_cast<void>(rng.between(10, 5));
  });
}

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------

TEST_CASE("pick() returns a mutable element", "[core][rng]") {
  dfr::prng rng{616};
  std::array<int, 4> items{0, 0, 0, 0};

  for (int i = 0; i < 100; ++i) {
    ++rng.pick(std::span{items});
  }
  CHECK(std::accumulate(items.begin(), items.end(), 0) == 100);
  for (const int count : items) {
    CHECK(count > 0);  // every element is reachable
  }
}

TEST_CASE("shuffle() permutes without losing elements", "[core][rng]") {
  dfr::prng rng{909};
  std::vector<int> items(32);
  std::iota(items.begin(), items.end(), 0);
  const std::vector<int> original = items;

  rng.shuffle(std::span{items});

  CHECK(items != original);  // it actually moved something
  std::vector<int> sorted = items;
  std::sort(sorted.begin(), sorted.end());
  CHECK(sorted == original);  // and lost nothing
}

TEST_CASE("shuffle() of a trivial range is a no-op costing nothing",
          "[core][rng]") {
  dfr::prng rng{5};
  const auto before = rng.save();

  std::vector<int> empty;
  std::vector<int> single{1};
  rng.shuffle(std::span{empty});
  rng.shuffle(std::span{single});

  CHECK(rng.save() == before);
  CHECK(single == std::vector<int>{1});
}

// ---------------------------------------------------------------------------
// State, and the determinism fingerprint
// ---------------------------------------------------------------------------

TEST_CASE("save and restore round-trip", "[core][rng]") {
  dfr::prng rng{77};
  take(rng, 10);

  const auto checkpoint = rng.save();
  const auto after_checkpoint = take(rng, 20);

  rng.restore(checkpoint);
  CHECK(rng.save() == checkpoint);
  CHECK(take(rng, 20) == after_checkpoint);

  // A restored generator continues the same stream, which is what lets a
  // simulation be rewound to a checkpoint and re-run.
  dfr::prng resumed{checkpoint};
  CHECK(take(resumed, 20) == after_checkpoint);
}

TEST_CASE("draws() is a determinism fingerprint", "[core][rng]") {
  // FoundationDB's "unseed" check in one integer: run a seed, record how many
  // words were consumed, run it again, compare. A mismatch proves something
  // took a different path through the code, which is the signature of a
  // determinism leak — and it shows up long before the outputs visibly diverge.
  const auto workload = [](std::uint64_t seed) {
    dfr::prng rng{seed};
    std::uint64_t sink = 0;
    for (int i = 0; i < 200; ++i) {
      sink += rng.below(rng.between(2, 97));
      if (rng.chance(dfr::percent(30))) {
        sink += rng.next();
      }
    }
    static_cast<void>(sink);
    return rng.draws();
  };

  CHECK(workload(1) == workload(1));
  CHECK(workload(2) == workload(2));

  // The count is path-dependent, so two different seeds are very unlikely to
  // agree. That is what makes it a useful fingerprint rather than a constant.
  CHECK(workload(1) != workload(2));
}

TEST_CASE("draws() can exceed the number of calls", "[core][rng]") {
  // Because Lemire's reduction rejects and retries. Documented here so that a
  // reader comparing draws() against a call count is not surprised.
  dfr::prng rng{13};
  for (int i = 0; i < 1000; ++i) {
    static_cast<void>(rng.below(3));
  }
  CHECK(rng.draws() >= 1000);
}

TEST_CASE("the generator is usable at compile time", "[core][rng]") {
  constexpr std::uint64_t first = [] {
    dfr::prng rng{42};
    return rng.next();
  }();
  STATIC_REQUIRE(first == 0xD0764D4F4476689FULL);

  constexpr std::uint64_t bounded = [] {
    dfr::prng rng{42};
    return rng.below(100);
  }();
  STATIC_REQUIRE(bounded < 100);
}

TEST_CASE("the state is small enough to checkpoint freely", "[core][rng]") {
  // The reason for not using std::mt19937_64, which carries 2,504 bytes: a
  // simulation checkpoints its generator on every determinism self-check, and
  // that cost is paid per check rather than per run.
  STATIC_REQUIRE(sizeof(dfr::prng::state) == 40);
  STATIC_REQUIRE(sizeof(dfr::prng) == 40);
}

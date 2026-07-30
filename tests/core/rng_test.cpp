#include <dfr/core/rng.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <set>
#include <vector>

namespace {

// Reference output of xoshiro256++ with SplitMix64 seeding, computed by an
// independent implementation (Python, arbitrary-precision arithmetic) rather
// than by this code.
//
// This is the test that matters most in the file. Every other property here
// would still hold if the engine were subtly wrong — a wrong rotation constant
// still produces a deterministic, well-distributed stream. Only a
// known-answer test against an outside implementation catches that, and only a
// known-answer test makes a recorded seed portable to another machine.
constexpr std::array<std::uint64_t, 4> kSeed0First4 = {
    0x53175D61490B23DFULL, 0x61DA6F3DC380D507ULL, 0x5C0FDF91EC9A7BFCULL,
    0x02EEBF8C3BBE5E1AULL};

constexpr std::array<std::uint64_t, 4> kSeed42First4 = {
    0xD0764D4F4476689FULL, 0x519E4174576F3791ULL, 0xFBE07CFB0C24ED8CULL,
    0xB37D9F600CD835B8ULL};

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
// The engine
// ---------------------------------------------------------------------------

TEST_CASE("the engine matches an independent implementation",
          "[core][rng]") {
  {
    dfr::prng rng{0};
    for (const std::uint64_t expected : kSeed0First4) {
      CHECK(rng.next() == expected);
    }
  }
  {
    dfr::prng rng{42};
    for (const std::uint64_t expected : kSeed42First4) {
      CHECK(rng.next() == expected);
    }
  }
}

TEST_CASE("seed zero produces a usable stream", "[core][rng]") {
  // Feeding the raw seed into the state directly would make seed 0 an all-zero
  // xoshiro state, from which the engine can never escape. SplitMix64 exists to
  // prevent that, so zero is worth its own test rather than being avoided.
  dfr::prng rng{0};
  const auto values = take(rng, 16);

  const std::set<std::uint64_t> distinct{values.begin(), values.end()};
  CHECK(distinct.size() == values.size());
  CHECK(std::accumulate(values.begin(), values.end(), std::uint64_t{0}) != 0);
}

TEST_CASE("the same seed replays exactly", "[core][rng]") {
  dfr::prng a{12345};
  dfr::prng b{12345};

  CHECK(take(a, 100) == take(b, 100));
  CHECK(a.draws() == b.draws());
}

TEST_CASE("different seeds diverge immediately", "[core][rng]") {
  dfr::prng a{1};
  dfr::prng b{2};

  // Not just "eventually different": SplitMix64 must decorrelate adjacent
  // seeds, because a fault-schedule search walks seeds 1, 2, 3, ... and
  // adjacent seeds producing similar streams would make the search explore
  // almost nothing.
  CHECK(a.next() != b.next());
  CHECK(take(a, 50) != take(b, 50));
}

// ---------------------------------------------------------------------------
// 128-bit multiply — the primitive a wrong fallback would silently corrupt
// ---------------------------------------------------------------------------

TEST_CASE("the portable multiply agrees with the intrinsic one",
          "[core][rng]") {
  // Only MSVC ever executes the portable path in production, so without this
  // test a wrong version of it would ship undetected and bias every bounded
  // draw on that one platform.
  dfr::prng rng{0xABCD};
  for (int i = 0; i < 2000; ++i) {
    const std::uint64_t a = rng.next();
    const std::uint64_t b = rng.next();
    CHECK(dfr::detail::wide_multiply(a, b) ==
          dfr::detail::wide_multiply_portable(a, b));
  }
}

TEST_CASE("the multiply matches externally computed products",
          "[core][rng]") {
  // Values from an arbitrary-precision reference, so both implementations are
  // checked against something outside this file.
  struct sample {
    std::uint64_t a, b, high, low;
  };
  constexpr std::array<sample, 4> samples = {{
      {0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL,
       0xFFFF'FFFF'FFFF'FFFEULL, 0x0000'0000'0000'0001ULL},
      {0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL,
       0x0121'FA00'AD77'D742ULL, 0x2236'D88F'E561'8CF0ULL},
      {0xDEAD'BEEF'CAFE'BABEULL, 3ULL, 2ULL, 0x9C09'3CCF'60FC'303AULL},
      {0x8000'0000'0000'0000ULL, 2ULL, 1ULL, 0ULL},
  }};

  for (const auto& s : samples) {
    CHECK(dfr::detail::wide_multiply(s.a, s.b) ==
          dfr::detail::wide_product{s.high, s.low});
    CHECK(dfr::detail::wide_multiply_portable(s.a, s.b) ==
          dfr::detail::wide_product{s.high, s.low});
  }
}

// ---------------------------------------------------------------------------
// Bounded draws
// ---------------------------------------------------------------------------

TEST_CASE("below() stays inside its bound", "[core][rng]") {
  dfr::prng rng{7};
  for (std::uint64_t bound : {2ULL, 3ULL, 7ULL, 1000ULL, (1ULL << 40)}) {
    for (int i = 0; i < 500; ++i) {
      CHECK(rng.below(bound) < bound);
    }
  }
}

TEST_CASE("below(1) consumes nothing from the stream", "[core][rng]") {
  dfr::prng rng{99};
  const auto before = rng.save();

  CHECK(rng.below(1) == 0);

  // A bound of one needs no randomness, and taking some would mean that
  // narrowing a range to a single element during shrinking shifts every draw
  // after it — which would make a reduced counterexample stop reproducing.
  CHECK(rng.save() == before);
  CHECK(rng.draws() == 0);
}

TEST_CASE("below() is unbiased across its range", "[core][rng]") {
  // 7 does not divide 2^64, so a modulo reduction would over-represent the low
  // residues. The tolerance is loose because this is a fixed-seed sanity check,
  // not a statistical test; it is here to catch a reduction that is grossly
  // wrong, such as one that can never return the top value.
  constexpr std::uint64_t kBound = 7;
  constexpr int kDraws = 70'000;
  constexpr int kExpected = kDraws / static_cast<int>(kBound);

  dfr::prng rng{2024};
  std::array<int, kBound> counts{};
  for (int i = 0; i < kDraws; ++i) {
    ++counts[rng.below(kBound)];
  }

  for (const int count : counts) {
    CHECK(count > kExpected * 9 / 10);
    CHECK(count < kExpected * 11 / 10);
  }
  CHECK(std::accumulate(counts.begin(), counts.end(), 0) == kDraws);
}

TEST_CASE("between() is inclusive on both ends", "[core][rng]") {
  // Inclusive because every bound this library cares about is: sequence
  // numbers, byte offsets, block counts. Both endpoints must actually be
  // reachable, which an off-by-one in the reduction would break at one end only.
  dfr::prng rng{555};
  bool saw_low = false;
  bool saw_high = false;

  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t v = rng.between(10, 12);
    CHECK(v >= 10);
    CHECK(v <= 12);
    saw_low = saw_low || v == 10;
    saw_high = saw_high || v == 12;
  }
  CHECK(saw_low);
  CHECK(saw_high);
}

TEST_CASE("between() handles degenerate and full ranges", "[core][rng]") {
  dfr::prng rng{1};

  const auto before = rng.save();
  CHECK(rng.between(42, 42) == 42);
  CHECK(rng.save() == before);  // a single-element range consumes nothing

  // The full 64-bit range needs no reduction at all, so it must cost exactly
  // one draw rather than entering the rejection loop.
  const auto full = rng.between(0, UINT64_MAX);
  static_cast<void>(full);
  CHECK(rng.draws() == 1);
}

TEST_CASE("index() produces a valid subscript", "[core][rng]") {
  dfr::prng rng{31337};
  const std::array<int, 5> items{10, 20, 30, 40, 50};

  std::set<std::size_t> seen;
  for (int i = 0; i < 500; ++i) {
    const std::size_t idx = rng.index(items.size());
    REQUIRE(idx < items.size());
    seen.insert(idx);
  }
  CHECK(seen.size() == items.size());  // every slot is reachable
}

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

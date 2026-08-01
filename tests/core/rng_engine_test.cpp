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

// Reference output of xoshiro256++ with SplitMix64 seeding, computed by an
// independent implementation (Python, arbitrary-precision arithmetic) rather
// than by this code.
//
// This is the test that matters most in the file. Every other property here
// would still hold if the engine were subtly wrong: a wrong rotation constant
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
// 128-bit multiply: the primitive a wrong fallback would silently corrupt
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
  constexpr std::array<sample, 4> kSamples = {{
      {0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL,
       0xFFFF'FFFF'FFFF'FFFEULL, 0x0000'0000'0000'0001ULL},
      {0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL,
       0x0121'FA00'AD77'D742ULL, 0x2236'D88F'E561'8CF0ULL},
      {0xDEAD'BEEF'CAFE'BABEULL, 3ULL, 2ULL, 0x9C09'3CCF'60FC'303AULL},
      {0x8000'0000'0000'0000ULL, 2ULL, 1ULL, 0ULL},
  }};

  for (const auto& s : kSamples) {
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
  for (const std::uint64_t bound : {2ULL, 3ULL, 7ULL, 1000ULL, (1ULL << 40)}) {
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
  // after it, which would make a reduced counterexample stop reproducing.
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

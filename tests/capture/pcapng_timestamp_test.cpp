#include <dfr/capture/pcapng/timestamp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ng = dfr::capture::pcapng;

TEST_CASE("the default resolution is microseconds, not nanoseconds",
          "[capture][pcapng][timestamp]") {
  // The if_tsresol option is absent from most files, and the format says absent
  // means microseconds. A reader that defaults to nanoseconds is off by a
  // thousand on the majority of real captures, with no symptom until two of them
  // are compared against each other.
  const ng::tick_resolution defaulted;
  CHECK(defaulted.raw() == 6);
  CHECK(defaulted.to_nanoseconds(1) == 1'000);
  CHECK(defaulted.to_nanoseconds(1'500'000) == 1'500'000'000);
  CHECK(defaulted.exact_in_nanoseconds());
}

TEST_CASE("decimal resolutions coarser than a nanosecond are exact",
          "[capture][pcapng][timestamp]") {
  struct sample {
    std::uint8_t raw;
    std::uint64_t ticks;
    std::uint64_t nanoseconds;
  };
  const std::array<sample, 6> cases{{
      {0, 1, 1'000'000'000},  // seconds
      {3, 1, 1'000'000},      // milliseconds
      {6, 1, 1'000},          // microseconds
      {9, 1, 1},              // nanoseconds
      {6, 0, 0},
      {9, 1'234'567'890, 1'234'567'890},
  }};

  for (const auto& c : cases) {
    const ng::tick_resolution r{c.raw};
    CHECK(r.to_nanoseconds(c.ticks) == c.nanoseconds);
    CHECK(r.exact_in_nanoseconds());
    CHECK_FALSE(r.binary());
  }
}

TEST_CASE("decimal resolutions finer than a nanosecond truncate, and say so",
          "[capture][pcapng][timestamp]") {
  // Picoseconds. The result cannot be represented exactly in nanoseconds, so it
  // is truncated, and exact_in_nanoseconds() reports that, which is what lets a
  // caller comparing captures know it is looking at rounded values.
  const ng::tick_resolution picos{12};
  CHECK_FALSE(picos.exact_in_nanoseconds());
  CHECK(picos.to_nanoseconds(1'000) == 1);
  CHECK(picos.to_nanoseconds(1'999) == 1);
  CHECK(picos.to_nanoseconds(999) == 0);
}

TEST_CASE("the high bit selects a power of two", "[capture][pcapng][timestamp]") {
  // 2^-10 of a second per tick. Rare in practice, and worth testing precisely
  // because the arithmetic needs a 128-bit intermediate and a wrong version
  // produces plausible timestamps rather than obvious nonsense.
  const ng::tick_resolution binary{ng::tick_resolution::kBinaryFlag | 10};
  CHECK(binary.binary());
  CHECK(binary.exponent() == 10);
  CHECK_FALSE(binary.exact_in_nanoseconds());

  // 1024 ticks is exactly one second.
  CHECK(binary.to_nanoseconds(1'024) == 1'000'000'000);
  CHECK(binary.to_nanoseconds(512) == 500'000'000);
  CHECK(binary.to_nanoseconds(1) == 976'562);  // 1e9 / 1024, truncated
}

TEST_CASE("a binary resolution keeps precision at large tick counts",
          "[capture][pcapng][timestamp]") {
  // This is the case the 128-bit intermediate exists for. A 64-bit
  // multiply-then-shift overflows well before this, and the wrong answer looks
  // like a plausible timestamp rather than an obvious error.
  const ng::tick_resolution binary{ng::tick_resolution::kBinaryFlag | 30};

  // 2^30 ticks is one second at this resolution, so 2^40 ticks is 1024 seconds.
  const std::uint64_t ticks = 1ULL << 40;
  CHECK(binary.to_nanoseconds(ticks) == 1'024ULL * 1'000'000'000ULL);

  // And a value large enough that ticks * 1e9 exceeds 64 bits: 2^40 * 1e9 is
  // about 1.1e21, well past 1.8e19.
  const std::uint64_t big = (1ULL << 50);
  CHECK(binary.to_nanoseconds(big) == (1ULL << 20) * 1'000'000'000ULL);
}

TEST_CASE("a binary resolution of zero is seconds",
          "[capture][pcapng][timestamp]") {
  const ng::tick_resolution seconds{ng::tick_resolution::kBinaryFlag | 0};
  CHECK(seconds.to_nanoseconds(1) == 1'000'000'000);
  CHECK(seconds.to_nanoseconds(3) == 3'000'000'000);
}

TEST_CASE("resolutions compare by their raw value",
          "[capture][pcapng][timestamp]") {
  CHECK(ng::tick_resolution{6} == ng::tick_resolution{});
  CHECK_FALSE(ng::tick_resolution{6} == ng::tick_resolution{9});

  // A decimal 6 and a binary 6 are different resolutions that share a low byte,
  // so they must not compare equal.
  CHECK_FALSE(ng::tick_resolution{6} ==
              ng::tick_resolution{ng::tick_resolution::kBinaryFlag | 6});
}

TEST_CASE("the conversions are usable at compile time",
          "[capture][pcapng][timestamp]") {
  STATIC_REQUIRE(ng::tick_resolution{}.to_nanoseconds(1) == 1'000);
  STATIC_REQUIRE(ng::tick_resolution{9}.to_nanoseconds(42) == 42);
  STATIC_REQUIRE(
      ng::tick_resolution{ng::tick_resolution::kBinaryFlag | 10}.to_nanoseconds(
          1'024) == 1'000'000'000);
}

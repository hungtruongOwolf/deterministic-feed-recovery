// Generating a schedule from a seed: determinism, the mask, and placement.
//
// This is where the load-bearing property lives: generation must consume a draw
// count that depends only on the seed, not on the stream length and not on which
// fault kinds are permitted, so it gets its own file rather than sharing one with
// the hand-editing API.

#include <dfr/chaos/schedule.hpp>

#include "chaos/support/schedule_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

namespace chaos = dfr::chaos;

using dfr_test::chaos::entries;
using dfr_test::chaos::generate;

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_CASE("the same seed produces the same schedule", "[chaos][schedule]") {
  // The property everything else rests on. Without it a failing run cannot be
  // reproduced from a seed, and the whole approach collapses.
  CHECK(generate(4711) == generate(4711));
  CHECK(entries(generate(4711)) == entries(generate(4711)));
}

TEST_CASE("different seeds produce different schedules", "[chaos][schedule]") {
  // Not a strict requirement of correctness, but if adjacent seeds produced
  // similar schedules a seed sweep would explore almost nothing.
  int differing = 0;
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    if (!(generate(seed) == generate(seed + 1))) {
      ++differing;
    }
  }
  CHECK(differing >= 18);
}

TEST_CASE("generation consumes a draw count that depends only on the seed",
          "[chaos][schedule][regression]") {
  // The load-bearing property for shrinking. If the number of draws depended on
  // the mask or on the stream length, then narrowing either one would shift every
  // subsequent draw and a reduced counterexample would stop reproducing.
  const auto draws_for = [](std::uint64_t stream, chaos::op_mask mask) {
    dfr::prng rng{99};
    chaos::schedule_options options;
    options.permitted = mask;
    chaos::schedule out;
    static_cast<void>(chaos::schedule::generate(rng, options, stream).get(out));
    return rng.draws();
  };

  const std::uint64_t baseline = draws_for(1'000, chaos::op_mask{});

  // A longer stream: same draws, because burst length is clamped rather than
  // redrawn.
  CHECK(draws_for(100'000, chaos::op_mask{}) == baseline);

  // A narrower mask: same draws, because the permitted kinds are collected once
  // and indexed rather than drawn and rejected.
  chaos::op_mask narrow;
  narrow.disable(chaos::fault_op::rewrite_session);
  CHECK(draws_for(1'000, narrow) == baseline);
}

// ---------------------------------------------------------------------------
// The mask
// ---------------------------------------------------------------------------

TEST_CASE("an empty mask permits everything", "[chaos][schedule]") {
  // Stored as disabled bits so that all-zero means all-permitted. Shrinking
  // therefore moves toward a more permissive configuration, and a minimal
  // counterexample reads as "this happens with everything enabled" rather than as
  // something contrived.
  const chaos::op_mask open;
  CHECK(open.raw() == 0);
  for (std::size_t i = 1; i < chaos::kFaultOpCount; ++i) {
    CHECK(open.permits(static_cast<chaos::fault_op>(i)));
  }
}

TEST_CASE("a mask restricts which kinds appear", "[chaos][schedule]") {
  chaos::schedule_options options;
  options.permitted = chaos::op_mask::only({chaos::fault_op::drop});
  options.max_faults = 32;

  const auto s = generate(7, options);
  REQUIRE_FALSE(s.empty());
  for (const auto& entry : s.faults()) {
    CHECK(entry.op == chaos::fault_op::drop);
  }
}

TEST_CASE("disable and enable round-trip", "[chaos][schedule]") {
  chaos::op_mask mask;
  mask.disable(chaos::fault_op::drop);
  CHECK_FALSE(mask.permits(chaos::fault_op::drop));
  CHECK(mask.permits(chaos::fault_op::duplicate));

  mask.enable(chaos::fault_op::drop);
  CHECK(mask.permits(chaos::fault_op::drop));
  CHECK(mask == chaos::op_mask{});
}

TEST_CASE("a mask permitting nothing is rejected", "[chaos][schedule]") {
  chaos::schedule_options options;
  options.permitted = chaos::op_mask::only({});

  dfr::prng rng{1};
  const auto result = chaos::schedule::generate(rng, options, 1'000);
  CHECK_FALSE(result.has_value());
  CHECK(result.error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

TEST_CASE("faults respect the warmup and the stream end",
          "[chaos][schedule][regression]") {
  // The warmup exists because a receiver has to establish its position before a
  // gap means anything. A fault on packet zero is reported as joining
  // mid-stream: true, and useless for testing recovery.
  chaos::schedule_options options;
  options.max_faults = 64;
  options.warmup_packets = 10;

  for (std::uint64_t seed = 1; seed <= 50; ++seed) {
    const auto s = generate(seed, options, /*stream=*/200);
    for (const auto& entry : s.faults()) {
      CHECK(entry.first_packet >= 10);
      CHECK(entry.last_packet() <= 199);
      CHECK(entry.packet_count >= 1);
    }
  }
}

TEST_CASE("a stream shorter than the warmup gets an empty schedule",
          "[chaos][schedule]") {
  // Not an error: a caller sweeping seeds against a short capture should get
  // nothing to do rather than a failure.
  chaos::schedule_options options;
  options.warmup_packets = 100;

  CHECK(generate(1, options, /*stream=*/50).empty());
  CHECK(generate(1, options, /*stream=*/100).empty());
  CHECK_FALSE(generate(1, options, /*stream=*/101).empty());

  // The threshold accounts for min_burst too, because min_burst is honoured by
  // constraining placement rather than clamped away afterwards.
  chaos::schedule_options wide;
  wide.warmup_packets = 100;
  wide.min_burst = 5;
  wide.max_burst = 5;
  CHECK(generate(1, wide, /*stream=*/104).empty());
  CHECK_FALSE(generate(1, wide, /*stream=*/105).empty());
}

TEST_CASE("burst lengths honour their bounds", "[chaos][schedule]") {
  chaos::schedule_options options;
  options.max_faults = 64;
  options.min_burst = 3;
  options.max_burst = 7;

  bool saw_min = false;
  bool saw_max = false;
  for (std::uint64_t seed = 1; seed <= 50; ++seed) {
    // The schedule must outlive the span, so it is named rather than iterated
    // straight off a temporary.
    const auto s = generate(seed, options, 10'000);
    for (const auto& entry : s.faults()) {
      CHECK(entry.packet_count >= 3);
      CHECK(entry.packet_count <= 7);
      // min_burst is a floor, honoured even for a fault at the very end of the
      // stream. The first implementation clamped instead, and this test is what
      // caught it.
      saw_min = saw_min || entry.packet_count == 3;
      saw_max = saw_max || entry.packet_count == 7;
    }
  }
  CHECK(saw_min);
  CHECK(saw_max);
}

TEST_CASE("bursts are the default, not single packets",
          "[chaos][schedule][regression]") {
  // BUILD-GUIDE.md section 5 is emphatic: model bursts, not probabilities. A
  // drop_probability of 0.001 tests almost nothing real, because consecutive loss
  // is what happens on a feed and what breaks a receiver's recovery window.
  chaos::schedule_options options;
  options.max_faults = 64;

  int multi_packet = 0;
  int total = 0;
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    const auto s = generate(seed, options, 10'000);
    for (const auto& entry : s.faults()) {
      ++total;
      if (entry.packet_count > 1) {
        ++multi_packet;
      }
    }
  }
  REQUIRE(total > 0);
  // With the default range of 1..10 the large majority must be bursts.
  CHECK(multi_packet * 2 > total);
}

TEST_CASE("invalid options are rejected", "[chaos][schedule]") {
  dfr::prng rng{1};

  chaos::schedule_options inverted;
  inverted.min_faults = 5;
  inverted.max_faults = 2;
  CHECK(chaos::schedule::generate(rng, inverted, 1'000).error_code() ==
        dfr::error::invalid_argument);

  chaos::schedule_options zero_burst;
  zero_burst.min_burst = 0;
  CHECK(chaos::schedule::generate(rng, zero_burst, 1'000).error_code() ==
        dfr::error::invalid_argument);

  chaos::schedule_options too_many;
  too_many.max_faults = chaos::kMaxFaults + 1;
  CHECK(chaos::schedule::generate(rng, too_many, 1'000).error_code() ==
        dfr::error::capacity_exceeded);
}


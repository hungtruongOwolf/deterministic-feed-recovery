#include <dfr/chaos/schedule.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

namespace chaos = dfr::chaos;

namespace {

chaos::schedule generate(std::uint64_t seed,
                         const chaos::schedule_options& options = {},
                         std::uint64_t stream = 1'000) {
  dfr::prng rng{seed};
  chaos::schedule out;
  REQUIRE(chaos::schedule::generate(rng, options, stream).get(out) ==
          dfr::error::ok);
  return out;
}

std::vector<chaos::fault> entries(const chaos::schedule& s) {
  return {s.faults().begin(), s.faults().end()};
}

}  // namespace

// ---------------------------------------------------------------------------
// The fault record
// ---------------------------------------------------------------------------

TEST_CASE("a fault covers exactly its burst", "[chaos][schedule]") {
  const chaos::fault burst{.op = chaos::fault_op::drop,
                           .first_packet = 100,
                           .packet_count = 3};

  CHECK_FALSE(burst.covers(99));
  CHECK(burst.covers(100));
  CHECK(burst.covers(101));
  CHECK(burst.covers(102));
  CHECK_FALSE(burst.covers(103));
  CHECK(burst.last_packet() == 102);
}

TEST_CASE("coverage cannot be defeated by underflow", "[chaos][schedule]") {
  // `packet_index - first_packet` on unsigned values wraps for an index below the
  // start, so the subtraction is guarded by the comparison before it. Without
  // that, every packet before the burst would appear to be inside it.
  const chaos::fault burst{.op = chaos::fault_op::drop,
                           .first_packet = 1'000,
                           .packet_count = 2};
  CHECK_FALSE(burst.covers(0));
  CHECK_FALSE(burst.covers(999));
  CHECK(burst.covers(1'000));
}

TEST_CASE("mutating operations are classified correctly",
          "[chaos][schedule]") {
  // The injector uses this to decide whether it needs a writable copy. A wrong
  // answer means either copying every packet or writing through a const view.
  CHECK_FALSE(chaos::mutates_bytes(chaos::fault_op::drop));
  CHECK_FALSE(chaos::mutates_bytes(chaos::fault_op::duplicate));
  CHECK_FALSE(chaos::mutates_bytes(chaos::fault_op::delay));

  CHECK(chaos::mutates_bytes(chaos::fault_op::flip_bit));
  CHECK(chaos::mutates_bytes(chaos::fault_op::truncate));
  CHECK(chaos::mutates_bytes(chaos::fault_op::overstate_block_count));
  CHECK(chaos::mutates_bytes(chaos::fault_op::rewrite_sequence));
  CHECK(chaos::mutates_bytes(chaos::fault_op::rewrite_session));
}

TEST_CASE("only a session rewrite is expected to be fatal",
          "[chaos][schedule]") {
  // This is the injector's half of the oracle: knowing the expected consequence
  // is what lets a test assert the receiver detected exactly the injected faults.
  CHECK(chaos::expects_fatal_report(chaos::fault_op::rewrite_session));
  CHECK_FALSE(chaos::expects_fatal_report(chaos::fault_op::drop));
  CHECK_FALSE(chaos::expects_fatal_report(chaos::fault_op::rewrite_sequence));
}

TEST_CASE("every operation has a distinct name", "[chaos][schedule]") {
  std::set<std::string_view> seen;
  for (std::size_t i = 0; i < chaos::kFaultOpCount; ++i) {
    const auto name = chaos::to_string(static_cast<chaos::fault_op>(i));
    CHECK_FALSE(name.empty());
    CHECK(name != "<unknown fault_op>");
    CHECK(seen.insert(name).second);
  }
}

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
  // mid-stream — true, and useless for testing recovery.
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

// ---------------------------------------------------------------------------
// Shrinking
// ---------------------------------------------------------------------------

TEST_CASE("removing a fault leaves the others untouched",
          "[chaos][schedule][regression]") {
  // The shrinker's whole basis. Every fault names an absolute packet index, so
  // deleting one does not move another. A per-packet decision stream cannot do
  // this: disabling one draw shifts every draw after it and the reduced case stops
  // reproducing.
  chaos::schedule_options options;
  options.min_faults = 6;
  options.max_faults = 6;

  const auto original = generate(1234, options);
  REQUIRE(original.size() == 6);

  for (std::size_t victim = 0; victim < original.size(); ++victim) {
    auto reduced = original;
    REQUIRE(reduced.remove(victim).has_value());
    REQUIRE(reduced.size() == 5);

    // Every survivor is bit-identical, and in the same order.
    std::size_t out = 0;
    for (std::size_t i = 0; i < original.size(); ++i) {
      if (i == victim) {
        continue;
      }
      CHECK(reduced.faults()[out] == original.faults()[i]);
      ++out;
    }
  }
}

TEST_CASE("removal is order-preserving", "[chaos][schedule]") {
  chaos::schedule s;
  for (std::uint64_t i = 0; i < 4; ++i) {
    REQUIRE(s.add(chaos::fault{.op = chaos::fault_op::drop,
                               .first_packet = i * 10,
                               .packet_count = 1})
                .has_value());
  }

  REQUIRE(s.remove(1).has_value());
  REQUIRE(s.size() == 3);
  CHECK(s.faults()[0].first_packet == 0);
  CHECK(s.faults()[1].first_packet == 20);
  CHECK(s.faults()[2].first_packet == 30);

  CHECK(s.remove(3).error_code() == dfr::error::invalid_argument);
}

TEST_CASE("a schedule can be written by hand", "[chaos][schedule]") {
  // The second reason a schedule is data: a regression test for a specific defect
  // names the fault it needs instead of hunting for a seed that produces it.
  chaos::schedule s;
  REQUIRE(s.add(chaos::fault{.op = chaos::fault_op::overstate_block_count,
                             .first_packet = 42,
                             .packet_count = 1,
                             .detail = 3})
              .has_value());

  const auto* found = s.at_packet(42);
  REQUIRE(found != nullptr);
  CHECK(found->op == chaos::fault_op::overstate_block_count);
  CHECK(found->detail == 3);
  CHECK(s.at_packet(41) == nullptr);
  CHECK(s.at_packet(43) == nullptr);
}

TEST_CASE("at_packet finds the first covering fault", "[chaos][schedule]") {
  // One fault per packet, and the schedule says which. Two composing in
  // array order would be a confusing thing to reason about in a counterexample.
  chaos::schedule s;
  REQUIRE(s.add(chaos::fault{.op = chaos::fault_op::drop,
                             .first_packet = 10,
                             .packet_count = 5})
              .has_value());
  REQUIRE(s.add(chaos::fault{.op = chaos::fault_op::duplicate,
                             .first_packet = 12,
                             .packet_count = 5})
              .has_value());

  CHECK(s.at_packet(12)->op == chaos::fault_op::drop);   // the earlier entry wins
  CHECK(s.at_packet(15)->op == chaos::fault_op::duplicate);
  CHECK(s.last_affected_packet() == 16);
}

TEST_CASE("a schedule fills to its capacity and then refuses",
          "[chaos][schedule]") {
  chaos::schedule s;
  for (std::size_t i = 0; i < chaos::kMaxFaults; ++i) {
    REQUIRE(s.add(chaos::fault{.op = chaos::fault_op::drop,
                               .first_packet = i,
                               .packet_count = 1})
                .has_value());
  }
  CHECK(s.size() == chaos::kMaxFaults);
  CHECK(s.add(chaos::fault{.op = chaos::fault_op::drop, .packet_count = 1})
            .error_code() == dfr::error::capacity_exceeded);
}

TEST_CASE("adding a no-op fault is a programmer error", "[chaos][schedule]") {
  DFR_CHECK_ABORTS({
    chaos::schedule s;
    static_cast<void>(s.add(chaos::fault{.op = chaos::fault_op::none,
                                         .packet_count = 1}));
  });
  DFR_CHECK_ABORTS({
    chaos::schedule s;
    static_cast<void>(s.add(chaos::fault{.op = chaos::fault_op::drop,
                                         .packet_count = 0}));
  });
}

TEST_CASE("an empty schedule affects nothing", "[chaos][schedule]") {
  const chaos::schedule s;
  CHECK(s.empty());
  CHECK(s.size() == 0);
  CHECK(s.at_packet(0) == nullptr);
  CHECK(s.last_affected_packet() == 0);
}

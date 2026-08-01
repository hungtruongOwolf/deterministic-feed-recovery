// The schedule as a container: shrinking, hand-writing, lookup and capacity.
//
// The half of the API a human uses. Shrinking by deletion is the reason a schedule
// is a flat array of absolute indices rather than a stream of per-packet decisions,
// and a hand-written schedule is how a regression test names the fault it needs
// instead of hunting for a seed that produces it.

#include <dfr/chaos/schedule.hpp>

#include "chaos/support/schedule_fixture.hpp"
#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace chaos = dfr::chaos;

using dfr_test::chaos::generate;

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
  // NOLINTNEXTLINE(readability-container-size-empty): a fact about size() in its own right, not a roundabout
  // emptiness check: this is a hand-rolled container, and the two accessors agreeing is exactly the kind of
  // thing this project has found genuinely diverging before (see moldudp64's cursor and builder).
  CHECK(s.size() == 0);
  CHECK(s.at_packet(0) == nullptr);
  CHECK(s.last_affected_packet() == 0);
}

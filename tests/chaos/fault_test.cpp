// The fault record: what one fault means on its own.
//
// No schedule and no seed here: only the vocabulary type and the questions a
// caller asks of it: which packets does it cover, does it change bytes, and is its
// consequence expected to be fatal. Split from the schedule tests because a reader
// changing the fault enum never needs the placement arithmetic.

#include <dfr/chaos/fault.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string_view>

namespace chaos = dfr::chaos;

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


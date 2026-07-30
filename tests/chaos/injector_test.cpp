// Faults that change *which* packets arrive and *when*: drop, duplicate, delay.
//
// The damage faults — the ones that rewrite bytes — live in
// injector_damage_test.cpp. This file never decodes a packet; it only watches the
// sequence of emissions, which is the whole of what a delivery fault means.

#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/target.hpp>

#include "chaos/support/injector_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chaos = dfr::chaos;

using dfr_test::chaos::collect;
using dfr_test::chaos::iex_injector;
using dfr_test::chaos::iextp_stream;
using dfr_test::chaos::one_fault;

// ---------------------------------------------------------------------------
// Pass-through
// ---------------------------------------------------------------------------

TEST_CASE("an empty schedule passes every packet unchanged",
          "[chaos][injector]") {
  const auto stream = iextp_stream(20);
  iex_injector injector;

  const auto got = collect(injector, stream);
  REQUIRE(got.size() == stream.size());
  for (std::size_t i = 0; i < stream.size(); ++i) {
    CHECK(got[i].bytes == stream[i]);
    CHECK(got[i].cause == chaos::fault_op::none);
    CHECK(got[i].source_index == i);
  }
  CHECK(injector.stats().mutated == 0);
  CHECK(injector.stats().dropped == 0);
}

// ---------------------------------------------------------------------------
// Delivery faults
// ---------------------------------------------------------------------------

TEST_CASE("a drop removes exactly its burst", "[chaos][injector]") {
  const auto stream = iextp_stream(20);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::drop,
                                               .first_packet = 5,
                                               .packet_count = 3})};

  const auto got = collect(injector, stream);
  CHECK(got.size() == stream.size() - 3);
  CHECK(injector.stats().dropped == 3);

  // Packets 5, 6 and 7 are gone and nothing else moved.
  std::vector<std::uint64_t> indices;
  for (const auto& e : got) {
    indices.push_back(e.source_index);
  }
  CHECK(indices == std::vector<std::uint64_t>{0, 1, 2, 3, 4, 8, 9, 10, 11, 12,
                                              13, 14, 15, 16, 17, 18, 19});
}

TEST_CASE("a duplicate emits the packet twice, marked", "[chaos][injector]") {
  const auto stream = iextp_stream(10);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::duplicate,
                                               .first_packet = 3,
                                               .packet_count = 1})};

  const auto got = collect(injector, stream);
  REQUIRE(got.size() == stream.size() + 1);

  CHECK(got[3].source_index == 3);
  CHECK_FALSE(got[3].is_duplicate);
  CHECK(got[4].source_index == 3);
  CHECK(got[4].is_duplicate);
  CHECK(got[4].cause == chaos::fault_op::duplicate);
  CHECK(got[3].bytes == got[4].bytes);
}

TEST_CASE("a delay moves a packet later without losing it",
          "[chaos][injector]") {
  const auto stream = iextp_stream(10);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::delay,
                                               .first_packet = 2,
                                               .packet_count = 1,
                                               .parameter = 3})};

  const auto got = collect(injector, stream);
  // Nothing is lost: a delay is a reorder, not a drop.
  CHECK(got.size() == stream.size());
  CHECK(injector.stats().delayed == 1);

  std::vector<std::uint64_t> indices;
  for (const auto& e : got) {
    indices.push_back(e.source_index);
  }
  // Packet 2 is released when index 5 is offered, and released *before* it, which
  // is the ordering a real reorder produces: the late packet arrives out of
  // position rather than overtaking anything.
  CHECK(indices == std::vector<std::uint64_t>{0, 1, 3, 4, 2, 5, 6, 7, 8, 9});
}

TEST_CASE("a delay past the end of the stream is still delivered",
          "[chaos][injector][regression]") {
  // Without flush(), a packet delayed past the last index would simply vanish —
  // which is a drop, not a reorder, and would make an oracle expect the wrong
  // thing.
  const auto stream = iextp_stream(5);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::delay,
                                               .first_packet = 3,
                                               .packet_count = 1,
                                               .parameter = 100})};

  const auto got = collect(injector, stream);
  CHECK(got.size() == stream.size());
  CHECK(got.back().source_index == 3);
  CHECK(got.back().cause == chaos::fault_op::delay);
}

TEST_CASE("delayed packets keep their relative order", "[chaos][injector]") {
  // Two packets delayed to the same release point must come out in the order they
  // were sent, which is what the in-place compaction of the queue preserves.
  const auto stream = iextp_stream(12);
  chaos::schedule plan;
  REQUIRE(plan.add(chaos::fault{.op = chaos::fault_op::delay,
                                .first_packet = 2,
                                .packet_count = 2,
                                .parameter = 4})
              .has_value());
  iex_injector injector{plan};

  const auto got = collect(injector, stream);
  std::vector<std::uint64_t> delayed;
  for (const auto& e : got) {
    if (e.cause == chaos::fault_op::delay) {
      delayed.push_back(e.source_index);
    }
  }
  CHECK(delayed == std::vector<std::uint64_t>{2, 3});
}

TEST_CASE("the delay queue is bounded and reports when it is full",
          "[chaos][injector]") {
  // TIGER_STYLE: put a limit on everything. When the queue cannot hold a packet
  // it is delivered on time and the fault counted as not applied, because a silent
  // drop would be a different fault from the one the schedule named.
  const auto stream = iextp_stream(200);
  chaos::schedule plan;
  REQUIRE(plan.add(chaos::fault{.op = chaos::fault_op::delay,
                                .first_packet = 0,
                                .packet_count = 100,
                                .parameter = 150})
              .has_value());
  iex_injector injector{plan};

  const auto got = collect(injector, stream);
  CHECK(got.size() == stream.size());  // nothing lost
  CHECK(injector.stats().delayed == chaos::kMaxDelayedPackets);
  CHECK(injector.stats().not_applicable ==
        100 - chaos::kMaxDelayedPackets);
}

// ---------------------------------------------------------------------------
// Determinism and the lifetime contract
// ---------------------------------------------------------------------------

TEST_CASE("the same schedule injects identically twice",
          "[chaos][injector]") {
  const auto stream = iextp_stream(40);
  dfr::prng rng{2024};
  chaos::schedule plan;
  REQUIRE(chaos::schedule::generate(rng, {}, stream.size()).get(plan) ==
          dfr::error::ok);

  iex_injector first{plan};
  iex_injector second{plan};
  const auto a = collect(first, stream);
  const auto b = collect(second, stream);

  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].bytes == b[i].bytes);
    CHECK(a[i].cause == b[i].cause);
    CHECK(a[i].source_index == b[i].source_index);
  }
  CHECK(first.stats() == second.stats());
}

TEST_CASE("an unmutated packet is emitted without a copy",
          "[chaos][injector]") {
  // The zero-copy path is what the lifetime contract buys. A packet the schedule
  // did not touch must be the caller's own bytes, not a copy in scratch.
  const auto stream = iextp_stream(4);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::flip_bit,
                                               .first_packet = 2,
                                               .packet_count = 1,
                                               .parameter = 0,
                                               .detail = 0})};

  std::vector<const void*> addresses;
  const auto emit = [&](const chaos::emission& e) {
    addresses.push_back(e.packet.data());
  };
  for (std::uint64_t i = 0; i < stream.size(); ++i) {
    REQUIRE(injector
                .offer(dfr::packet_view{stream[i].data(), stream[i].size()}, i,
                       emit)
                .has_value());
  }
  REQUIRE(injector.flush(emit).has_value());

  REQUIRE(addresses.size() == stream.size());
  for (std::size_t i = 0; i < stream.size(); ++i) {
    if (i == 2) {
      CHECK(addresses[i] != stream[i].data());  // mutated: lives in scratch
    } else {
      CHECK(addresses[i] == stream[i].data());  // untouched: zero copy
    }
  }
}

TEST_CASE("statistics account for every offered packet",
          "[chaos][injector]") {
  const auto stream = iextp_stream(60);
  dfr::prng rng{77};
  chaos::schedule plan;
  REQUIRE(chaos::schedule::generate(rng, {}, stream.size()).get(plan) ==
          dfr::error::ok);

  iex_injector injector{plan};
  const auto got = collect(injector, stream);

  const auto& s = injector.stats();
  CHECK(s.offered == stream.size());
  CHECK(s.emitted == got.size());

  // Every offered packet either came out, was dropped, or is accounted for by a
  // duplicate adding one. flush() releases anything still delayed, so nothing is
  // in flight by the time this runs.
  CHECK(injector.delayed_in_flight() == 0);
  CHECK(s.emitted + s.dropped == s.offered + s.duplicated);
}

TEST_CASE("both targets satisfy the concept", "[chaos][injector]") {
  STATIC_REQUIRE(chaos::fault_target<chaos::iextp_target>);
  STATIC_REQUIRE(chaos::fault_target<chaos::moldudp64_target>);

  struct not_a_target {};
  STATIC_REQUIRE_FALSE(chaos::fault_target<not_a_target>);
}

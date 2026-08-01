// The oracle, second half: with a retransmit server, every injected fault is repaired.
//
// The strongest statement the library can make about itself. Faults injected into a verified
// stream are detected, requested and repaired, leaving the merged output complete with every
// message delivered exactly once, and "exactly once" matters as much as completeness, because
// a client that delivered a message twice when a retransmit crossed a late copy would corrupt
// a book just as thoroughly as one that dropped it.

#include "integration/support/oracle_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace chaos = dfr::chaos;
namespace rec = dfr::recovery;

using dfr_test::integration::at_us;
using dfr_test::integration::clean_stream;
using dfr_test::integration::derivable_faults;
using dfr_test::integration::drain;
using dfr_test::integration::drain_to_quiet;
using dfr_test::integration::ledger;
using dfr_test::integration::offer_if_intact;
using dfr_test::integration::oracle_client;
using dfr_test::integration::oracle_options;
using dfr_test::integration::run;
using dfr_test::integration::serve_retransmit;

// ---------------------------------------------------------------------------
// Repair: the loop closes
// ---------------------------------------------------------------------------

TEST_CASE("with a retransmit server, every injected fault is repaired",
          "[integration][oracle]") {
  // The strongest statement the library can make about itself: faults injected into a
  // verified stream are detected, requested, and repaired, leaving the merged output
  // complete and every message delivered exactly once.
  std::uint64_t seeds_that_needed_repair = 0;

  for (std::uint64_t seed = 1; seed <= 120; ++seed) {
    const auto result = run(seed, 300, /*serve=*/true);
    REQUIRE_FALSE(result.record.client_left_live);

    REQUIRE(result.missing_at_end == 0);
    REQUIRE(result.holes.empty());

    // Exactly once matters as much as completeness. A retransmit crossing with a late copy
    // must not deliver a message a second time, or the book is corrupt in the other
    // direction.
    REQUIRE(result.record.delivered_more_than_once().empty());

    // Contiguous from the first delivery to the high-water mark, with nothing skipped.
    for (std::uint64_t s = result.low; s < result.high; ++s) {
      REQUIRE(result.record.deliveries.at(s) == 1);
    }

    if (result.record.retransmits_served > 0) {
      ++seeds_that_needed_repair;
    }
  }

  CHECK(seeds_that_needed_repair > 60);
}

TEST_CASE("repair does not depend on how badly the stream was damaged",
          "[integration][oracle]") {
  // Bursts of up to forty consecutive packets, which is well past the point where a
  // receiver handling only single-packet loss would fall over. The same two properties
  // still have to hold.
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const auto stream = clean_stream(400);

    chaos::schedule plan;
    dfr::prng rng{seed};
    chaos::schedule_options options;
    options.min_faults = 2;
    options.max_faults = 4;
    options.min_burst = 5;
    options.max_burst = 40;
    options.permitted = derivable_faults();
    REQUIRE(chaos::schedule::generate(rng, options, 400).get(plan) ==
            dfr::error::ok);

    chaos::injector<chaos::iextp_target> injector{plan};
    oracle_client client{oracle_options()};
    ledger record;
    std::int64_t clock_us = 0;

    const auto emit = [&](const chaos::emission& e) {
      clock_us += 20;
      offer_if_intact(client, record, e.packet, at_us(clock_us));
      drain(client, record, stream, clock_us);
    };

    for (std::uint64_t i = 0; i < stream.size(); ++i) {
      const dfr::packet_view view{stream[i].bytes.data(), stream[i].bytes.size()};
      REQUIRE(injector.offer(view, i, emit).has_value());
    }
    REQUIRE(injector.flush(emit).has_value());
    drain_to_quiet(client, record, stream, clock_us);

    REQUIRE_FALSE(record.client_left_live);
    REQUIRE(client.total_missing() == 0);
    REQUIRE(record.delivered_more_than_once().empty());
  }
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_CASE("the whole pipeline is reproducible from a seed",
          "[integration][oracle]") {
  // The claim the README makes about the project. If it did not hold, none of the failures
  // this suite can find would be investigable.
  for (const std::uint64_t seed : {std::uint64_t{7}, std::uint64_t{4711},
                                   std::uint64_t{999'331}}) {
    const auto first = run(seed, 250, /*serve=*/true);
    const auto second = run(seed, 250, /*serve=*/true);

    CHECK(first.record.packets_offered == second.record.packets_offered);
    CHECK(first.record.packets_discarded == second.record.packets_discarded);
    CHECK(first.record.retransmits_served == second.record.retransmits_served);
    CHECK(first.record.deliveries == second.record.deliveries);
    CHECK(first.high == second.high);
    CHECK(first.injected == second.injected);
  }
}

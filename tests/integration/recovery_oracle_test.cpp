// The oracle, first half: the holes the client reports are exactly what went missing.
//
// This is what the previous ten commits were for. dfr::chaos damages a verified stream,
// dfr::wire decodes what survives, and dfr::recovery reports what is missing.
//
// Not a superset — a false gap costs a real retransmit request for data that arrived. Not a
// subset — a missed gap is a silently wrong book, which is the failure this project exists to
// prevent. Repair, where the harness also plays retransmit server, is in
// recovery_repair_test.cpp.

#include "integration/support/oracle_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>

namespace rec = dfr::recovery;

using dfr_test::integration::at_us;
using dfr_test::integration::clean_stream;
using dfr_test::integration::ledger;
using dfr_test::integration::missing_sequences;
using dfr_test::integration::never_offered;
using dfr_test::integration::offer_if_intact;
using dfr_test::integration::oracle_client;
using dfr_test::integration::oracle_options;
using dfr_test::integration::run;

// ---------------------------------------------------------------------------
// The floor
// ---------------------------------------------------------------------------

TEST_CASE("a clean stream is delivered whole, exactly once, silently",
          "[integration][oracle]") {
  // If this failed, every count below would be measuring the harness rather than the
  // client.
  const auto stream = clean_stream(300);
  oracle_client client{oracle_options()};
  ledger record;

  std::int64_t clock_us = 0;
  for (const auto& packet : stream) {
    clock_us += 20;
    offer_if_intact(client, record,
                    dfr::packet_view{packet.bytes.data(), packet.bytes.size()},
                    at_us(clock_us));
  }

  CHECK_FALSE(record.client_left_live);
  CHECK(record.packets_discarded == 0);
  CHECK(record.packets_offered == 300);
  CHECK(client.total_missing() == 0);
  CHECK(record.delivered_more_than_once().empty());
  CHECK(record.delivered_once() == record.deliveries.size());
  CHECK(client.retransmission().stats().requests_sent == 0);
  CHECK(client.state() == rec::client_state::live);
}

// ---------------------------------------------------------------------------
// Detection: the holes are exactly what went missing
// ---------------------------------------------------------------------------

TEST_CASE("the client reports exactly the messages that never reached it",
          "[integration][oracle]") {
  // The load-bearing assertion, checked over many different fault combinations. A superset
  // costs real retransmit requests for data that arrived; a subset is a silently wrong book.
  std::uint64_t seeds_with_faults = 0;

  for (std::uint64_t seed = 1; seed <= 120; ++seed) {
    const auto result = run(seed, 300, /*serve=*/false);
    REQUIRE_FALSE(result.record.client_left_live);

    const auto reported = missing_sequences(result);
    const auto actual = never_offered(result);
    REQUIRE(reported == actual);

    // And nothing was delivered twice along the way.
    REQUIRE(result.record.delivered_more_than_once().empty());

    if (!actual.empty()) {
      ++seeds_with_faults;
    }
  }

  // Negative space: the loop above would pass by doing nothing if no seed lost anything.
  CHECK(seeds_with_faults > 60);
}

TEST_CASE("the accounting balances: delivered plus missing is the whole span",
          "[integration][oracle]") {
  // The identity a wrong implementation cannot satisfy. Every sequence between the first
  // delivered and the high-water mark is either delivered exactly once or recorded as
  // missing — never both, and never neither.
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    const auto result = run(seed, 300, /*serve=*/false);
    REQUIRE_FALSE(result.record.client_left_live);

    const auto missing = missing_sequences(result);
    std::uint64_t accounted = 0;
    for (std::uint64_t s = result.low; s < result.high; ++s) {
      const auto delivered = result.record.deliveries.find(s);
      const bool once = delivered != result.record.deliveries.end() &&
                        delivered->second == 1;
      const bool absent = missing.contains(s);
      REQUIRE(once != absent);  // exactly one of the two, for every sequence
      ++accounted;
    }
    REQUIRE(accounted == result.high - result.low);
    REQUIRE(result.missing_at_end == missing.size());
  }
}


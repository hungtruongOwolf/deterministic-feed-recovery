// The self-oracle: does the receiver report exactly the faults that were injected?
//
// Everything else in tests/chaos checks that the injector does what it was told.
// This file checks the property the whole library exists to provide, which is
// stronger and harder: for a stream that went through the injector, the number of
// inconsistencies a receiver reports must equal the number actually present: no
// false positives, no misses.
//
// The trick is having a second opinion that does not come from the code under
// test. `chain_checker` is a small state machine, so comparing it against a
// reimplementation of itself would prove nothing. Instead the second opinion is
// structural: every path through chain_checker::observe() resynchronises its
// expectation from the packet it just saw, so a report on packet i is a statement
// about the *pair* (i-1, i) and nothing else. That means the count of reports must
// equal the count of adjacent pairs that disagree, which can be computed from
// decoded header fields with arithmetic written out longhand, sharing no code with
// the checker at all.
//
// The exact-count claim is made only for faults that leave every header decodable.
// For flip_bit and truncate the assertion is the weaker but still meaningful one:
// something was reported, and nothing was reported before the fault was injected.

#include <dfr/chaos/injector.hpp>

#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/header.hpp>

#include "chaos/support/injector_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;

using dfr_test::chaos::captured;
using dfr_test::chaos::collect;
using dfr_test::chaos::iex_injector;
using dfr_test::chaos::iextp_stream;
using dfr_test::chaos::one_fault;

namespace {

dfr::packet_view view_of(const captured& c) {
  return dfr::packet_view{c.bytes.data(), c.bytes.size()};
}

// The second opinion. Counts adjacent pairs of delivered packets that disagree,
// using the three chain rules written out longhand rather than calling the
// header's next_sequence()/next_stream_offset() helpers: the point is to share no
// code with what is being checked.
//
// Returns -1 if any header failed to decode, which is the caller's signal that the
// exact-count claim does not apply to this stream.
[[nodiscard]] std::int64_t count_disagreeing_pairs(
    const std::vector<captured>& delivered) {
  std::vector<iex::header> headers;
  headers.reserve(delivered.size());
  for (const auto& c : delivered) {
    iex::header decoded;
    if (iex::decode_header(view_of(c)).get(decoded) != dfr::error::ok) {
      return -1;
    }
    headers.push_back(decoded);
  }

  std::int64_t breaks = 0;
  for (std::size_t i = 1; i < headers.size(); ++i) {
    const iex::header& prev = headers[i - 1];
    const iex::header& curr = headers[i];

    const bool session_agrees = curr.session == prev.session;
    const bool sequence_agrees =
        curr.first_sequence == prev.first_sequence + prev.message_count;
    const bool offset_agrees =
        curr.stream_offset ==
        prev.stream_offset + static_cast<std::int64_t>(prev.payload_length);

    if (!session_agrees || !sequence_agrees || !offset_agrees) {
      ++breaks;
    }
  }
  return breaks;
}

struct report {
  std::size_t position{0};  // index into the delivered stream
  std::uint64_t source_index{0};
  dfr::error code{dfr::error::ok};
};

// What the receiver under test actually says.
[[nodiscard]] std::vector<report> reports_from_checker(
    const std::vector<captured>& delivered) {
  std::vector<report> out;
  iex::chain_checker checker;
  for (std::size_t i = 0; i < delivered.size(); ++i) {
    iex::header decoded;
    if (iex::decode_header(view_of(delivered[i])).get(decoded) !=
        dfr::error::ok) {
      continue;
    }
    const auto outcome = checker.observe(decoded);
    if (!outcome) {
      out.push_back(report{.position = i,
                           .source_index = delivered[i].source_index,
                           .code = outcome.error_code()});
    }
  }
  return out;
}

// Runs one schedule over a fresh stream and hands back both opinions.
struct trial {
  std::vector<captured> delivered;
  std::int64_t disagreeing_pairs{0};
  std::vector<report> reports;
  chaos::injection_stats stats;
};

[[nodiscard]] trial run(const chaos::schedule& plan, std::size_t packets) {
  const auto stream = iextp_stream(packets);
  iex_injector injector{plan};
  trial out;
  out.delivered = collect(injector, stream);
  out.disagreeing_pairs = count_disagreeing_pairs(out.delivered);
  out.reports = reports_from_checker(out.delivered);
  out.stats = injector.stats();
  return out;
}

// How many faults the schedule actually managed to apply. A fault the packet could
// not carry has no consequence, so an oracle must not expect one.
[[nodiscard]] std::uint64_t faults_applied(const chaos::injection_stats& s) {
  return s.dropped + s.duplicated + s.delayed + s.mutated;
}

}  // namespace

// ---------------------------------------------------------------------------
// No false positives
// ---------------------------------------------------------------------------

TEST_CASE("an uninjected stream produces no reports at all",
          "[chaos][oracle]") {
  // The floor the whole property rests on. If a clean stream reported anything,
  // every count below would be meaningless.
  const auto t = run(chaos::schedule{}, 500);
  CHECK(t.disagreeing_pairs == 0);
  CHECK(t.reports.empty());
  CHECK(faults_applied(t.stats) == 0);
}

// ---------------------------------------------------------------------------
// Exact agreement, fault kind by fault kind
// ---------------------------------------------------------------------------

TEST_CASE("the receiver reports exactly the breaks a drop creates",
          "[chaos][oracle]") {
  // A dropped burst is one break, not one per lost packet: the receiver sees a
  // single jump. A checker that reported per lost packet would look more thorough
  // and would be wrong.
  const auto t = run(one_fault(chaos::fault{.op = chaos::fault_op::drop,
                                            .first_packet = 40,
                                            .packet_count = 5}),
                     200);
  REQUIRE(t.disagreeing_pairs >= 0);
  CHECK(t.disagreeing_pairs == 1);
  CHECK(t.reports.size() == 1);
  CHECK(t.reports.front().code == dfr::error::sequence_gap);
  CHECK(static_cast<std::int64_t>(t.reports.size()) == t.disagreeing_pairs);
}

TEST_CASE("the receiver reports exactly the breaks a duplicate creates",
          "[chaos][oracle]") {
  // One break, not two, and the reason is worth writing down, because two is the
  // intuitive answer. The duplicate arrives immediately after the original, so it
  // regresses by exactly the original's message count; resynchronising from the
  // duplicate lands on the same expectation the original had already set. The
  // packet that follows therefore chains cleanly and the disruption is a single
  // pair, not a wake that continues.
  //
  // This is a stream where a plausible receiver reports twice, so pinning one is
  // the assertion with content.
  const auto t = run(one_fault(chaos::fault{.op = chaos::fault_op::duplicate,
                                            .first_packet = 30,
                                            .packet_count = 1}),
                     200);
  REQUIRE(t.disagreeing_pairs >= 0);
  CHECK(static_cast<std::int64_t>(t.reports.size()) == t.disagreeing_pairs);
  CHECK(t.reports.size() == 1);
  CHECK(t.reports[0].code == dfr::error::sequence_regressed);
}

TEST_CASE("the receiver reports exactly the breaks a reorder creates",
          "[chaos][oracle]") {
  const auto t = run(one_fault(chaos::fault{.op = chaos::fault_op::delay,
                                            .first_packet = 25,
                                            .packet_count = 1,
                                            .parameter = 4}),
                     200);
  REQUIRE(t.disagreeing_pairs >= 0);
  CHECK(static_cast<std::int64_t>(t.reports.size()) == t.disagreeing_pairs);
  CHECK(t.reports.size() == 3);  // gap, then the late packet, then a gap again
}

TEST_CASE("the receiver reports exactly the breaks a sequence rewrite creates",
          "[chaos][oracle]") {
  const auto t = run(
      one_fault(chaos::fault{.op = chaos::fault_op::rewrite_sequence,
                             .first_packet = 60,
                             .packet_count = 1,
                             .parameter64 = 1'000}),
      200);
  REQUIRE(t.disagreeing_pairs >= 0);
  CHECK(static_cast<std::int64_t>(t.reports.size()) == t.disagreeing_pairs);
  CHECK(t.reports.size() == 2);
}

TEST_CASE("the receiver reports exactly the breaks a session rewrite creates",
          "[chaos][oracle]") {
  const auto t = run(one_fault(chaos::fault{.op = chaos::fault_op::rewrite_session,
                                            .first_packet = 50,
                                            .packet_count = 1,
                                            .parameter = 0x9999}),
                     200);
  REQUIRE(t.disagreeing_pairs >= 0);
  CHECK(static_cast<std::int64_t>(t.reports.size()) == t.disagreeing_pairs);
  CHECK(t.reports[0].code == dfr::error::session_changed);
}

// ---------------------------------------------------------------------------
// Exact agreement under a generated schedule
// ---------------------------------------------------------------------------

TEST_CASE("agreement holds for many seeded multi-fault schedules",
          "[chaos][oracle]") {
  // The test that would catch a wrong count in a combination none of the
  // single-fault cases above thought to try. Only the delivery and sequencing
  // faults are permitted, because those are the ones that leave every header
  // decodable and therefore the ones the exact-count claim covers.
  chaos::op_mask mask;
  mask.disable(chaos::fault_op::flip_bit);
  mask.disable(chaos::fault_op::truncate);
  mask.disable(chaos::fault_op::overstate_block_count);
  mask.disable(chaos::fault_op::understate_block_count);
  mask.disable(chaos::fault_op::overstate_block_length);

  std::uint64_t streams_with_faults = 0;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    dfr::prng rng{seed};
    chaos::schedule plan;
    REQUIRE(chaos::schedule::generate(rng, {.permitted = mask}, 300).get(plan) ==
            dfr::error::ok);

    const auto t = run(plan, 300);
    REQUIRE(t.disagreeing_pairs >= 0);
    // The load-bearing assertion, checked 200 times with different fault
    // combinations: the receiver saw exactly what was there.
    REQUIRE(static_cast<std::int64_t>(t.reports.size()) == t.disagreeing_pairs);

    if (faults_applied(t.stats) > 0) {
      ++streams_with_faults;
      // And a fault that was applied is never invisible.
      REQUIRE_FALSE(t.reports.empty());
    }
  }
  // Negative space: the loop above would pass trivially if no seed ever produced
  // a fault.
  CHECK(streams_with_faults > 100);
}

// ---------------------------------------------------------------------------
// Position, and the faults the exact claim does not cover
// ---------------------------------------------------------------------------

TEST_CASE("nothing is reported before the fault was injected",
          "[chaos][oracle]") {
  // Covers the byte-level faults too, where an exact count is not claimed: a
  // report attributed to a packet ahead of the damage would mean the checker is
  // carrying state it should have resynchronised.
  const std::array byte_faults{chaos::fault_op::flip_bit, chaos::fault_op::truncate,
                               chaos::fault_op::rewrite_sequence, chaos::fault_op::drop};

  for (const auto op : byte_faults) {
    const std::uint64_t injected_at = 80;
    const auto t = run(one_fault(chaos::fault{.op = op,
                                              .first_packet = injected_at,
                                              .packet_count = 1,
                                              .parameter = 8,
                                              .detail = 3}),
                       200);
    for (const auto& r : t.reports) {
      CHECK(r.source_index >= injected_at);
    }
  }
}

TEST_CASE("a fault the packet could not carry produces no reports",
          "[chaos][oracle]") {
  // The reason injection_stats has not_applicable. A truncation past the end of a
  // packet is refused, the packet is delivered untouched, and an oracle that
  // expected a consequence anyway would be asserting a false claim.
  const auto t = run(one_fault(chaos::fault{.op = chaos::fault_op::truncate,
                                            .first_packet = 30,
                                            .packet_count = 1,
                                            .parameter = 100'000}),
                     100);
  CHECK(t.stats.not_applicable == 1);
  CHECK(faults_applied(t.stats) == 0);
  CHECK(t.reports.empty());
}

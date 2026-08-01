// Faults that change the bytes: corruption, truncation, and lies about framing.
//
// Split from injector_test.cpp at the seam docs/STYLE.md §1.10 asks for: the
// delivery faults are checked by watching which packets come out, while these are
// checked by decoding what came out and asking the receiver what it thinks. The
// two need different assertions and different reasoning, so they read better apart.

#include <dfr/chaos/injector.hpp>

#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/header.hpp>

#include "chaos/support/injector_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;

using dfr_test::chaos::collect;
using dfr_test::chaos::iex_injector;
using dfr_test::chaos::iextp_stream;
using dfr_test::chaos::iextp_stream_of_two_blocks;
using dfr_test::chaos::one_fault;

// ---------------------------------------------------------------------------
// Corruption
// ---------------------------------------------------------------------------

TEST_CASE("flip_bit changes exactly one bit", "[chaos][injector]") {
  const auto stream = iextp_stream(10);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::flip_bit,
                                               .first_packet = 4,
                                               .packet_count = 1,
                                               .parameter = 7,
                                               .detail = 2})};

  const auto got = collect(injector, stream);
  REQUIRE(got.size() == stream.size());
  REQUIRE(got[4].bytes.size() == stream[4].size());
  CHECK(got[4].cause == chaos::fault_op::flip_bit);

  int differing_bytes = 0;
  for (std::size_t i = 0; i < stream[4].size(); ++i) {
    if (got[4].bytes[i] != stream[4][i]) {
      ++differing_bytes;
      CHECK(i == 7);
      const auto before = static_cast<std::uint8_t>(stream[4][i]);
      const auto after = static_cast<std::uint8_t>(got[4].bytes[i]);
      CHECK((before ^ after) == 0x04);  // bit 2
    }
  }
  CHECK(differing_bytes == 1);
}

TEST_CASE("truncate only ever shortens", "[chaos][injector][regression]") {
  // A "truncation" that lengthened the packet would be reading uninitialised
  // scratch: a determinism leak as well as the wrong fault. An out-of-range
  // length is counted as not applied instead.
  const auto stream = iextp_stream(6);

  {
    iex_injector shorten{one_fault(chaos::fault{.op = chaos::fault_op::truncate,
                                                .first_packet = 2,
                                                .packet_count = 1,
                                                .parameter = 20})};
    const auto got = collect(shorten, stream);
    CHECK(got[2].bytes.size() == 20);
    CHECK(got[2].bytes == stream[2].substr(0, 20));
  }
  {
    iex_injector too_long{one_fault(chaos::fault{.op = chaos::fault_op::truncate,
                                                 .first_packet = 2,
                                                 .packet_count = 1,
                                                 .parameter = 9'999})};
    const auto got = collect(too_long, stream);
    CHECK(got[2].bytes == stream[2]);  // untouched
    CHECK(too_long.stats().not_applicable == 1);
    CHECK(too_long.stats().mutated == 0);
  }
}

TEST_CASE("a fault a packet cannot carry is counted, not silently dropped",
          "[chaos][injector][regression]") {
  // The oracle depends on this. A test asserting "the receiver saw exactly the
  // injected faults" must be able to tell a fault that was applied from one that
  // could not be, and a silent drop would look like a delivery fault.
  const std::vector<std::string> tiny{std::string(4, 'x'), std::string(4, 'y')};
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::rewrite_sequence,
                                               .first_packet = 1,
                                               .packet_count = 1,
                                               .parameter64 = 5})};

  const auto got = collect(injector, tiny);
  CHECK(got.size() == 2);
  CHECK(got[1].bytes == tiny[1]);  // too short for the sequence field
  CHECK(injector.stats().not_applicable == 1);
  CHECK(injector.stats().mutated == 0);
}

// ---------------------------------------------------------------------------
// Protocol-aware rewrites
// ---------------------------------------------------------------------------

TEST_CASE("rewrite_sequence creates a gap the decoder can see",
          "[chaos][injector]") {
  const auto stream = iextp_stream(10);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::rewrite_sequence,
                                               .first_packet = 5,
                                               .packet_count = 1,
                                               .parameter64 = 500})};

  const auto got = collect(injector, stream);
  REQUIRE(got.size() == stream.size());

  const dfr::packet_view damaged{got[5].bytes.data(), got[5].bytes.size()};
  iex::header header;
  REQUIRE(iex::decode_header(damaged).get(header) == dfr::error::ok);
  // Packet 5 natively carries sequence 6, so the jump is visible.
  CHECK(header.first_sequence == 6 + 500);

  // And the chain checker reports it as a gap rather than as anything else.
  iex::chain_checker checker;
  dfr::error seen = dfr::error::ok;
  for (const auto& e : got) {
    iex::header h;
    REQUIRE(iex::decode_header(
                dfr::packet_view{e.bytes.data(), e.bytes.size()})
                .get(h) == dfr::error::ok);
    if (const auto outcome = checker.observe(h); !outcome && seen == dfr::error::ok) {
      seen = outcome.error_code();
    }
  }
  CHECK(seen == dfr::error::sequence_gap);
}

TEST_CASE("rewrite_session is the fatal fault", "[chaos][injector]") {
  const auto stream = iextp_stream(10);
  iex_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::rewrite_session,
                                               .first_packet = 6,
                                               .packet_count = 1,
                                               .parameter = 0x9999})};

  const auto got = collect(injector, stream);

  iex::chain_checker checker;
  bool saw_fatal = false;
  for (const auto& e : got) {
    iex::header h;
    REQUIRE(iex::decode_header(
                dfr::packet_view{e.bytes.data(), e.bytes.size()})
                .get(h) == dfr::error::ok);
    if (const auto outcome = checker.observe(h); !outcome && outcome.is_fatal()) {
      saw_fatal = true;
      CHECK(outcome.error_code() == dfr::error::session_changed);
    }
  }
  CHECK(saw_fatal);
  CHECK(chaos::expects_fatal_report(chaos::fault_op::rewrite_session));
}

TEST_CASE("overstating the block count breaks the framing chain only",
          "[chaos][injector][oracle]") {
  // The fault that exists because penberg/helix trusts the count. On IEX-TP,
  // Message Count is redundant with Payload Length, and only Message Count is
  // rewritten: deliberately. Rewriting both would make the packet
  // self-consistent and therefore uninteresting.
  const auto stream = iextp_stream(10);
  iex_injector injector{
      one_fault(chaos::fault{.op = chaos::fault_op::overstate_block_count,
                             .first_packet = 4,
                             .packet_count = 1,
                             .detail = 3})};

  const auto got = collect(injector, stream);
  const dfr::packet_view damaged{got[4].bytes.data(), got[4].bytes.size()};

  // The header still decodes, and the sequence chain is untouched.
  iex::header header;
  REQUIRE(iex::decode_header(damaged).get(header) == dfr::error::ok);
  CHECK(header.message_count == 4);  // one real block, three claimed

  // The framing chain is what catches it.
  const auto framing = iex::verify_payload_framing(damaged);
  CHECK_FALSE(framing.has_value());
  CHECK(framing.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("overstating the block length makes it overrun the payload",
          "[chaos][injector][oracle]") {
  const auto stream = iextp_stream(10);
  chaos::schedule plan;
  // Only the first block's declared length is raised; Payload Length is left
  // alone, so the block claims more bytes than the datagram says it holds.
  REQUIRE(plan.add(chaos::fault{.op = chaos::fault_op::overstate_block_length,
                                .first_packet = 4,
                                .packet_count = 1,
                                .detail = 5})
              .has_value());
  iex_injector injector{plan};

  const auto got = collect(injector, stream);
  const dfr::packet_view damaged{got[4].bytes.data(), got[4].bytes.size()};

  // The block now claims more bytes than the declared payload holds.
  const auto framing = iex::verify_payload_framing(damaged);
  CHECK_FALSE(framing.has_value());
  CHECK(framing.error_code() == dfr::error::block_overruns_datagram);
}

TEST_CASE("understating the block count leaves unconsumed payload",
          "[chaos][injector][oracle]") {
  // The mirror image of overstating, and a different failure: the walk finishes
  // early and bytes are left over. A receiver that stops when the count runs out
  // never notices, which is why the cursor reports trailing_bytes rather than
  // treating an exhausted count as success.
  //
  // Needs a packet carrying two blocks: understating a one-block packet lands on
  // zero, which is a legal empty packet rather than a lie about framing.
  const auto stream = iextp_stream_of_two_blocks(6);
  iex_injector injector{
      one_fault(chaos::fault{.op = chaos::fault_op::understate_block_count,
                             .first_packet = 2,
                             .packet_count = 1,
                             .detail = 1})};

  const auto got = collect(injector, stream);
  const dfr::packet_view damaged{got[2].bytes.data(), got[2].bytes.size()};

  iex::header header;
  REQUIRE(iex::decode_header(damaged).get(header) == dfr::error::ok);
  CHECK(header.message_count == 1);  // two real blocks, one admitted

  const auto framing = iex::verify_payload_framing(damaged);
  CHECK_FALSE(framing.has_value());
  CHECK(framing.error_code() == dfr::error::trailing_bytes);
}


// The other half of "one injector drives both MoldUDP64 and IEX-TP": chaos::moldudp64_target, actually run.
//
// Found by measuring coverage, not by reading: target.hpp's own header comment states the one-injector claim as
// the reason the whole policy-template design exists, and injector_test.cpp:252 has a
// STATIC_REQUIRE(fault_target<moldudp64_target>) proving the four functions exist with the right signatures.
// Neither of those runs a byte through them. Every actual fault-injection test in this suite (injector_test.cpp,
// injector_damage_test.cpp, oracle_test.cpp) drives chaos::injector<iextp_target>, so moldudp64_target's own
// arithmetic (the big-endian sequence add, the session bytes, the saturating count and length adjustments) had
// never executed until this file.

#include <dfr/chaos/injector.hpp>

#include <dfr/wire/moldudp64/header.hpp>

#include "chaos/support/injector_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace chaos = dfr::chaos;
namespace mold = dfr::wire::moldudp64;

using dfr_test::chaos::collect;
using dfr_test::chaos::moldudp64_injector;
using dfr_test::chaos::moldudp64_stream;
using dfr_test::chaos::one_fault;

TEST_CASE("rewrite_sequence adds to the big-endian sequence field",
          "[chaos][injector][moldudp64]") {
  const auto stream = moldudp64_stream(10);
  moldudp64_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::rewrite_sequence,
                                                     .first_packet = 5,
                                                     .packet_count = 1,
                                                     .parameter64 = 500})};

  const auto got = collect(injector, stream);
  REQUIRE(got.size() == stream.size());

  const dfr::packet_view damaged{got[5].bytes.data(), got[5].bytes.size()};
  mold::header header;
  REQUIRE(mold::decode_header(damaged).get(header) == dfr::error::ok);
  // Packet 5 natively carries sequence 6 (packets are 1 message each, starting at 1).
  CHECK(header.sequence == 6 + 500);
  CHECK(got[5].cause == chaos::fault_op::rewrite_sequence);
}

TEST_CASE("rewrite_session changes the last four bytes of the session field",
          "[chaos][injector][moldudp64]") {
  const auto stream = moldudp64_stream(3);
  moldudp64_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::rewrite_session,
                                                     .first_packet = 1,
                                                     .packet_count = 1,
                                                     .parameter = 0xABCD1234})};

  const auto got = collect(injector, stream);
  REQUIRE(got.size() == stream.size());

  const dfr::packet_view damaged{got[1].bytes.data(), got[1].bytes.size()};
  mold::header header;
  REQUIRE(mold::decode_header(damaged).get(header) == dfr::error::ok);
  const auto bytes = header.session.bytes();
  // The last four bytes carry the numeric id, big-endian; the first six stay the printable "SESS01".
  CHECK(static_cast<std::uint8_t>(bytes[6]) == 0xAB);
  CHECK(static_cast<std::uint8_t>(bytes[7]) == 0xCD);
  CHECK(static_cast<std::uint8_t>(bytes[8]) == 0x12);
  CHECK(static_cast<std::uint8_t>(bytes[9]) == 0x34);
  for (std::size_t i = 0; i < 6; ++i) {
    CHECK(static_cast<char>(bytes[i]) == "SESS01"[i]);
  }
}

TEST_CASE("overstate_block_count raises the declared message count",
          "[chaos][injector][moldudp64]") {
  const auto stream = moldudp64_stream(3);
  moldudp64_injector grown{one_fault(chaos::fault{.op = chaos::fault_op::overstate_block_count,
                                                  .first_packet = 0,
                                                  .packet_count = 1,
                                                  .detail = 4})};
  const auto grown_got = collect(grown, stream);
  mold::header grown_header;
  REQUIRE(mold::decode_header(
              dfr::packet_view{grown_got[0].bytes.data(), grown_got[0].bytes.size()})
              .get(grown_header) == dfr::error::ok);
  CHECK(grown_header.message_count == 1 + 4);
  CHECK(grown_got[0].cause == chaos::fault_op::overstate_block_count);
}

TEST_CASE("understate_block_count cannot push a packet below one message",
          "[chaos][injector][moldudp64]") {
  // A packet already at the natural floor of one message cannot be pushed below it: doing so would turn "the
  // count is a lie" into a heartbeat (message_count == 0), which is a different fault the oracle would
  // misclassify entirely.
  const auto stream = moldudp64_stream(3);
  moldudp64_injector shrunk{one_fault(chaos::fault{.op = chaos::fault_op::understate_block_count,
                                                   .first_packet = 0,
                                                   .packet_count = 1,
                                                   .detail = 10})};
  const auto shrunk_got = collect(shrunk, stream);
  mold::header shrunk_header;
  REQUIRE(mold::decode_header(
              dfr::packet_view{shrunk_got[0].bytes.data(), shrunk_got[0].bytes.size()})
              .get(shrunk_header) == dfr::error::ok);
  CHECK(shrunk_header.message_count == 1);
}

TEST_CASE("overstate_block_length changes the declared length of the first block",
          "[chaos][injector][moldudp64]") {
  const auto stream = moldudp64_stream(3);
  moldudp64_injector injector{one_fault(chaos::fault{.op = chaos::fault_op::overstate_block_length,
                                                     .first_packet = 2,
                                                     .packet_count = 1,
                                                     .detail = 9000})};

  const auto got = collect(injector, stream);
  REQUIRE(got.size() == stream.size());

  // The header decodes regardless (decode_header does not walk the blocks), and the length field sits
  // immediately after it. This is asserting the byte the fault actually wrote, the same way
  // injector_damage_test.cpp checks rewrite_sequence by decoding rather than by counting bytes changed.
  const auto& bytes = got[2].bytes;
  REQUIRE(bytes.size() >= mold::kHeaderSize + 2);
  const auto declared = static_cast<std::uint16_t>(
      (static_cast<std::uint8_t>(bytes[mold::kHeaderSize]) << 8) |
      static_cast<std::uint8_t>(bytes[mold::kHeaderSize + 1]));
  // "p2" is 2 bytes; saturating at UINT16_MAX means this clamps rather than wrapping.
  CHECK(declared == 2 + 9000);
}

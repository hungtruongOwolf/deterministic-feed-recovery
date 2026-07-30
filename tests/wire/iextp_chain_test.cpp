#include <dfr/wire/iextp.hpp>

#include <dfr/core/mutable_packet_view.hpp>

#include "support/death_test.hpp"
#include "wire/support/raw_iextp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace iex = dfr::wire::iextp;
using dfr_test::iex::as_text;
using dfr_test::iex::prototype;
using dfr_test::iex::raw_packet;

// The chains
// ---------------------------------------------------------------------------

TEST_CASE("sequence and stream offset chain independently", "[wire][iextp]") {
  const iex::header first{
      .payload_length = 12, .message_count = 3, .stream_offset = 100,
      .first_sequence = 1000};

  CHECK(first.next_sequence() == 1003);
  CHECK(first.next_stream_offset() == 112);
}

TEST_CASE("the chain checker accepts a well-formed sequence",
          "[wire][iextp]") {
  iex::chain_checker checker;
  CHECK_FALSE(checker.started());

  iex::header packet{.payload_length = 10,
                     .message_count = 2,
                     .stream_offset = 0,
                     .first_sequence = 1};

  for (int i = 0; i < 5; ++i) {
    REQUIRE(checker.observe(packet).has_value());
    packet.first_sequence = packet.next_sequence();
    packet.stream_offset = packet.next_stream_offset();
  }

  CHECK(checker.started());
  CHECK(checker.expected_sequence() == 11);
  CHECK(checker.expected_stream_offset() == 50);
}

TEST_CASE("the first packet establishes the chain rather than failing",
          "[wire][iextp]") {
  // A receiver joining a live feed mid-session must not report a spurious error
  // on its first packet.
  iex::chain_checker checker;
  const iex::header mid_session{.payload_length = 8,
                                .message_count = 1,
                                .stream_offset = 987'654,
                                .first_sequence = 555'000};

  CHECK(checker.observe(mid_session).has_value());
  CHECK(checker.expected_sequence() == 555'001);
}

TEST_CASE("a sequence gap is reported and then resynchronised",
          "[wire][iextp]") {
  iex::chain_checker checker;
  const iex::header first{
      .payload_length = 4, .message_count = 1, .stream_offset = 0,
      .first_sequence = 1};
  REQUIRE(checker.observe(first).has_value());

  // Jumps from expected 2 to 10.
  const iex::header jumped{
      .payload_length = 4, .message_count = 1, .stream_offset = 4,
      .first_sequence = 10};
  const auto gap = checker.observe(jumped);
  CHECK_FALSE(gap.has_value());
  CHECK(gap.error_code() == dfr::error::sequence_gap);
  CHECK_FALSE(gap.is_fatal());

  // Resynchronised, so one gap does not produce an error on every packet after
  // it — which would bury the next real fault in noise.
  const iex::header following{
      .payload_length = 4, .message_count = 1, .stream_offset = 8,
      .first_sequence = 11};
  CHECK(checker.observe(following).has_value());
}

TEST_CASE("a regressed sequence is distinguished from a gap",
          "[wire][iextp]") {
  // A duplicate, or the late half of an A/B pair. Both recoverable and usually
  // uninteresting, so they must not be reported as a gap that triggers recovery.
  iex::chain_checker checker;
  const iex::header first{
      .payload_length = 4, .message_count = 2, .stream_offset = 0,
      .first_sequence = 100};
  REQUIRE(checker.observe(first).has_value());

  const auto replayed = checker.observe(first);
  CHECK_FALSE(replayed.has_value());
  CHECK(replayed.error_code() == dfr::error::sequence_regressed);
  CHECK_FALSE(replayed.is_fatal());
}

TEST_CASE("a session change is fatal", "[wire][iextp]") {
  iex::chain_checker checker;
  const iex::header first{.session = 1,
                          .payload_length = 4,
                          .message_count = 1,
                          .stream_offset = 0,
                          .first_sequence = 1};
  REQUIRE(checker.observe(first).has_value());

  const iex::header new_session{.session = 2,
                                .payload_length = 4,
                                .message_count = 1,
                                .stream_offset = 0,
                                .first_sequence = 1};
  const auto changed = checker.observe(new_session);
  CHECK_FALSE(changed.has_value());
  CHECK(changed.error_code() == dfr::error::session_changed);
  CHECK(changed.is_fatal());
}

TEST_CASE("a broken offset chain is caught when the sequence chain is intact",
          "[wire][iextp][oracle]") {
  // The reason to build against IEX first, in one test.
  //
  // Sequence numbers chain perfectly and stream offsets do not, which is what a
  // corrupted Payload Length looks like. A receiver checking only sequence
  // numbers — which is all MoldUDP64 permits — sees nothing wrong and carries a
  // silently wrong byte position for the rest of the session.
  iex::chain_checker checker;
  const iex::header first{
      .payload_length = 10, .message_count = 1, .stream_offset = 0,
      .first_sequence = 1};
  REQUIRE(checker.observe(first).has_value());

  const iex::header offset_wrong{
      .payload_length = 10,
      .message_count = 1,
      .stream_offset = 999,  // should be 10
      .first_sequence = 2};  // chains correctly

  const auto broken = checker.observe(offset_wrong);
  CHECK_FALSE(broken.has_value());
  CHECK(broken.error_code() == dfr::error::message_length_mismatch);
}


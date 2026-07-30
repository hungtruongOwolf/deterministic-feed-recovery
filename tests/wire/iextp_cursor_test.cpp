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

// ---------------------------------------------------------------------------
// message_cursor
// ---------------------------------------------------------------------------

TEST_CASE("the cursor walks blocks and numbers them", "[wire][iextp]") {
  const auto packet = raw_packet{}
                          .first_sequence(500)
                          .count(3)
                          .block("aaa")
                          .block("bb")
                          .block("c")
                          .seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);
  CHECK(cursor.remaining() == 3);

  std::vector<std::uint64_t> sequences;
  std::vector<std::string_view> payloads;
  REQUIRE(cursor
              .drain([&](const iex::message& m) {
                sequences.push_back(m.sequence);
                payloads.push_back(as_text(m.payload));
              })
              .has_value());

  CHECK(sequences == std::vector<std::uint64_t>{500, 501, 502});
  CHECK(payloads == std::vector<std::string_view>{"aaa", "bb", "c"});
}

TEST_CASE("a heartbeat cursor is born exhausted", "[wire][iextp]") {
  const auto packet = raw_packet{}.first_sequence(9).count(0).seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);
  CHECK(cursor.done());
  CHECK(cursor.packet_header().kind() == iex::packet_kind::heartbeat);
}

TEST_CASE("the cursor trusts the declared payload length, not the datagram size",
          "[wire][iextp]") {
  // A switch may pad a short frame and a capture may truncate one. Trusting the
  // datagram length would decode the wrong number of blocks in either case.
  const auto padded = raw_packet{}
                          .first_sequence(1)
                          .count(1)
                          .block("ab")
                          .seal()      // payload length = 4
                          .raw_byte(0xFF)  // one byte of padding after it
                          .raw_byte(0xFF);

  const auto decoded = iex::message_cursor::over(padded.view());
  CHECK_FALSE(decoded.has_value());
  CHECK(decoded.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("a payload length longer than the datagram is rejected",
          "[wire][iextp]") {
  const auto packet =
      raw_packet{}.first_sequence(1).count(1).block("ab").payload_length(500);

  const auto decoded = iex::message_cursor::over(packet.view());
  CHECK_FALSE(decoded.has_value());
  CHECK(decoded.error_code() == dfr::error::block_overruns_datagram);
}

TEST_CASE("an overstated block count is caught", "[wire][iextp][regression]") {
  const auto packet =
      raw_packet{}.first_sequence(1).count(4).block("aa").block("bb").seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const iex::message&) { ++delivered; });
  CHECK(delivered == 2);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("a block whose length overruns the payload is caught",
          "[wire][iextp][regression]") {
  const auto packet = raw_packet{}
                          .first_sequence(1)
                          .count(1)
                          .declared_block(500, "short")
                          .seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  const auto only = cursor.next();
  CHECK_FALSE(only.has_value());
  CHECK(only.error_code() == dfr::error::block_overruns_datagram);
}

TEST_CASE("an understated count leaves unconsumed payload",
          "[wire][iextp][regression]") {
  const auto packet =
      raw_packet{}.first_sequence(1).count(1).block("aa").block("bb").seal();

  iex::message_cursor cursor;
  REQUIRE(iex::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const iex::message&) { ++delivered; });
  CHECK(delivered == 1);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("verify_payload_framing is the third chain", "[wire][iextp][oracle]") {
  const auto good =
      raw_packet{}.first_sequence(1).count(2).block("aa").block("bb").seal();
  CHECK(iex::verify_payload_framing(good.view()).has_value());

  const auto bad =
      raw_packet{}.first_sequence(1).count(1).block("aa").block("bb").seal();
  const auto outcome = iex::verify_payload_framing(bad.view());
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("next() on an exhausted cursor is a programmer error",
          "[wire][iextp]") {
  DFR_CHECK_ABORTS({
    const auto packet = raw_packet{}.count(0).seal();
    iex::message_cursor cursor;
    static_cast<void>(iex::message_cursor::over(packet.view()).get(cursor));
    static_cast<void>(cursor.next());
  });
}


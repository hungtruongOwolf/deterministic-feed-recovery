#include <dfr/wire/moldudp64.hpp>

#include "support/death_test.hpp"
#include "wire/support/raw_moldudp64.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mold = dfr::wire::moldudp64;
using dfr_test::mold::as_text;
using dfr_test::mold::raw_packet;
using dfr_test::mold::three_messages;

// ---------------------------------------------------------------------------
// The helix defect, on the real protocol
// ---------------------------------------------------------------------------

TEST_CASE("an overstated block count is caught, not trusted",
          "[wire][moldudp64][regression]") {
  // penberg/helix moldudp64.hh:106-115 loops `MessageCount` times performing a
  // reinterpret_cast with no bounds check. Here the header claims four blocks
  // and two are present. The cursor must report it on the third attempt rather
  // than reading whatever follows the buffer.
  const auto packet = raw_packet{}
                          .session("LIAR")
                          .sequence(50)
                          .count(4)
                          .block("aa")
                          .block("bb");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  mold::message m;
  REQUIRE(cursor.next().get(m) == dfr::error::ok);
  CHECK(m.sequence == 50);
  REQUIRE(cursor.next().get(m) == dfr::error::ok);
  CHECK(m.sequence == 51);

  REQUIRE_FALSE(cursor.done());  // the header still claims two more
  const auto third = cursor.next();
  CHECK_FALSE(third.has_value());
  CHECK(third.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("drain reports an overstated count", "[wire][moldudp64][regression]") {
  const auto packet =
      raw_packet{}.session("LIAR").sequence(1).count(9).block("only");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const mold::message&) { ++delivered; });

  CHECK(delivered == 1);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("a block whose declared length overruns is caught",
          "[wire][moldudp64][regression]") {
  // The other half of the same defect: the count is honest, the length is not.
  const auto packet = raw_packet{}
                          .session("LIAR")
                          .sequence(1)
                          .count(1)
                          .declared_block(0xFFFF, "short");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  const auto only = cursor.next();
  CHECK_FALSE(only.has_value());
  CHECK(only.error_code() == dfr::error::block_overruns_datagram);
}

TEST_CASE("a truncated length field is an overstated count",
          "[wire][moldudp64][regression]") {
  // One stray byte where a two-byte length should be. The header promised a
  // block and there is not even room for its length, so the count is the lie.
  const auto packet =
      raw_packet{}.session("S").sequence(1).count(1).raw_byte(0x00);

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  const auto only = cursor.next();
  CHECK_FALSE(only.has_value());
  CHECK(only.error_code() == dfr::error::block_count_overstated);
}

TEST_CASE("an understated count leaves trailing bytes",
          "[wire][moldudp64][regression]") {
  // The mirror image, and just as damaging: the header says one block, two are
  // present, and the second is silently dropped by any decoder that stops
  // counting. drain() is what catches it.
  const auto packet = raw_packet{}
                          .session("S")
                          .sequence(1)
                          .count(1)
                          .block("first")
                          .block("second");

  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(packet.view()).get(cursor) ==
          dfr::error::ok);

  int delivered = 0;
  const auto outcome = cursor.drain([&](const mold::message&) { ++delivered; });

  CHECK(delivered == 1);
  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::trailing_bytes);
}

TEST_CASE("calling next() on an exhausted cursor is a programmer error",
          "[wire][moldudp64]") {
  DFR_CHECK_ABORTS({
    const auto packet = raw_packet{}.session("S").sequence(1).count(0);
    mold::message_cursor cursor;
    static_cast<void>(mold::message_cursor::over(packet.view()).get(cursor));
    static_cast<void>(cursor.next());
  });
}


// Channel identity, and the two integers the type system keeps apart.

#include <dfr/recovery/channel.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace rec = dfr::recovery;

TEST_CASE("registering a channel assigns consecutive ids",
          "[recovery][channel]") {
  rec::channel_table table;
  REQUIRE(table.empty());

  rec::channel_id first;
  rec::channel_id second;
  REQUIRE(table.add(1'013).get(first) == dfr::error::ok);
  REQUIRE(table.add(32'001).get(second) == dfr::error::ok);

  CHECK(first.index() == 0);
  CHECK(second.index() == 1);
  CHECK(table.size() == 2);
  CHECK(table.wire_channel_of(first) == 1'013);
  CHECK(table.wire_channel_of(second) == 32'001);
}

TEST_CASE("registering the same channel twice returns the same id",
          "[recovery][channel]") {
  // A configuration listing one channel under two names is describing one channel.
  // Handing out a second id would split one feed's sequence state in two, and each
  // half would report the other half's messages as missing.
  rec::channel_table table;
  rec::channel_id a;
  rec::channel_id b;
  REQUIRE(table.add(7).get(a) == dfr::error::ok);
  REQUIRE(table.add(7).get(b) == dfr::error::ok);

  CHECK(a == b);
  CHECK(table.size() == 1);
}

TEST_CASE("an unregistered channel is not added on demand",
          "[recovery][channel]") {
  // Growing the table from wire data would let a corrupted channel field consume
  // configuration slots, and would make the receiver start tracking a stream nobody
  // asked it to follow, then report gaps in it.
  rec::channel_table table;
  rec::channel_id id;
  REQUIRE(table.add(7).get(id) == dfr::error::ok);

  const auto missing = table.find(999);
  CHECK_FALSE(missing.has_value());
  CHECK(missing.error_code() == dfr::error::not_supported);
  CHECK(table.size() == 1);
}

TEST_CASE("find returns the id assigned at configuration time",
          "[recovery][channel]") {
  rec::channel_table table;
  rec::channel_id added;
  REQUIRE(table.add(1'013).get(added) == dfr::error::ok);

  rec::channel_id found = rec::channel_id::at(7);  // deliberately wrong to start
  REQUIRE(table.find(1'013).get(found) == dfr::error::ok);
  CHECK(found == added);
}

TEST_CASE("the table fills to capacity and then refuses",
          "[recovery][channel]") {
  rec::channel_table table;
  for (std::uint32_t i = 0; i < rec::kMaxChannels; ++i) {
    rec::channel_id id;
    REQUIRE(table.add(100 + i).get(id) == dfr::error::ok);
  }
  const auto refused = table.add(9'999);
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::capacity_exceeded);
}

TEST_CASE("a channel id out of range aborts", "[recovery][channel]") {
  // The whole reason channel_id is not an integer. IEX's observed channel numbers
  // run into the thousands; using one as an array index is a silent out-of-bounds
  // read on a hot path, so the conversion is a checked one and happens once.
  DFR_CHECK_ABORTS((void)rec::channel_id::at(rec::kMaxChannels));
}

TEST_CASE("asking for the wire number of an unregistered id aborts",
          "[recovery][channel]") {
  DFR_CHECK_ABORTS({
    const rec::channel_table table;
    (void)table.wire_channel_of(rec::channel_id::at(0));
  });
}

TEST_CASE("the table is usable at compile time", "[recovery][channel]") {
  // Which means a configuration fixed at build time costs nothing at run time.
  static_assert([] {
    rec::channel_table table;
    rec::channel_id id;
    if (table.add(1'013).get(id) != dfr::error::ok) {
      return false;
    }
    return table.size() == 1 && table.wire_channel_of(id) == 1'013;
  }());
  SUCCEED("the static_assert above is the test");
}

// A snapshot that is bytes, consumed by a client that has only bytes.
//
// The point of serving Glimpse over a real protocol rather than handing over a struct is that it makes the one
// question a snapshot protocol has to answer testable: **how does the client learn where the snapshot ends and
// the live feed begins?** In a struct that is a field nobody can get wrong. On a wire it is a message that has to
// arrive, be recognised, and be believed.
//
// So the assertions here are about the join. A client with no access to the server's memory reads the frames,
// rebuilds a book, learns a resume position from an End Of Snapshot message, and must end up with the same book
// as one that never fell behind.

#include <dfr/book/order_book.hpp>
#include <dfr/venue/glimpse_service.hpp>
#include <dfr/wire/deep.hpp>
#include <dfr/wire/glimpse.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace book = dfr::book;
namespace deep = dfr::wire::deep;
namespace glimpse = dfr::wire::glimpse;
namespace soup = dfr::wire::soupbintcp;
namespace venue = dfr::venue;

namespace {

using test_book = book::order_book<16>;

constexpr std::uint32_t kSession = 0xBEEF;

deep::price at(std::int64_t dollars, std::int64_t ten_thousandths) {
  return deep::price{dollars * deep::kPriceScale + ten_thousandths};
}

deep::price_level_update quote(bool buy, deep::price level, std::uint32_t size) {
  deep::price_level_update out;
  out.buy = buy;
  out.symbol = "ZTEST";
  out.level = level;
  out.size = size;
  out.head.type = buy ? deep::message_type::price_level_buy : deep::message_type::price_level_sell;
  return out;
}

// A book with depth on both sides, so a snapshot has something to carry.
test_book a_book() {
  test_book b;
  REQUIRE(b.apply(quote(true, at(20, 8000), 100)));
  REQUIRE(b.apply(quote(true, at(20, 8500), 200)));
  REQUIRE(b.apply(quote(true, at(20, 9000), 300)));
  REQUIRE(b.apply(quote(false, at(20, 9500), 400)));
  REQUIRE(b.apply(quote(false, at(21, 0), 500)));
  return b;
}

// Everything the service emitted, as a client's socket would have received it: one byte stream.
std::string served(venue::glimpse_service& service, const test_book& state,
                   std::uint64_t next_sequence) {
  std::string stream;
  const auto emit = [&](dfr::packet_view frame) {
    stream.append(reinterpret_cast<const char*>(frame.data()), frame.size());
  };
  REQUIRE(service.serve(state, "ZTEST", next_sequence, emit).has_value());
  return stream;
}

// A client with nothing but the bytes: it frames, decodes, and rebuilds.
struct consumed {
  test_book state;
  std::uint64_t resume_from{0};
  bool saw_begin{false};
  bool saw_end{false};
  bool session_ended{false};
  std::uint64_t levels{0};
};

consumed consume(const std::string& stream) {
  consumed out;
  soup::stream_cursor cursor;
  std::size_t at_byte = 0;

  while (at_byte < stream.size()) {
    const dfr::packet_view rest{stream.data() + at_byte, stream.size() - at_byte};
    soup::sequenced_packet next;
    if (cursor.next(rest).get(next) != dfr::error::ok) {
      break;
    }
    at_byte += next.frame.frame_size;

    if (next.frame.type == soup::packet_type::login_accepted) {
      soup::login_accepted accepted;
      REQUIRE(soup::decode_login_accepted(next.frame.payload).get(accepted) == dfr::error::ok);
      cursor.accept_login(accepted.next_sequence);
      continue;
    }
    if (next.frame.type == soup::packet_type::end_of_session) {
      out.session_ended = true;
      continue;
    }
    if (next.frame.type != soup::packet_type::sequenced_data) {
      continue;
    }

    const auto body = next.frame.payload;
    if (body.empty()) {
      continue;
    }
    if (glimpse::is_glimpse_type(body.u8_at(0))) {
      glimpse::begin_snapshot begin;
      if (glimpse::decode_begin(body).get(begin) == dfr::error::ok) {
        REQUIRE(begin.session == kSession);
        out.saw_begin = true;
        continue;
      }
      glimpse::end_snapshot end;
      REQUIRE(glimpse::decode_end(body).get(end) == dfr::error::ok);
      REQUIRE(end.session == kSession);
      out.resume_from = end.next_sequence;
      out.saw_end = true;
      continue;
    }
    deep::price_level_update update;
    REQUIRE(deep::decode_price_level(body).get(update) == dfr::error::ok);
    REQUIRE(out.state.apply(update).has_value());
    ++out.levels;
  }
  return out;
}

}  // namespace

TEST_CASE("a client rebuilds the exact book from the bytes alone", "[integration][glimpse]") {
  const auto reference = a_book();
  venue::glimpse_service service{kSession};
  const auto got = consume(served(service, reference, /*next_sequence=*/500));

  CHECK(got.saw_begin);
  CHECK(got.saw_end);
  CHECK(got.session_ended);
  CHECK(got.levels == 5);
  // The assertion the file exists for: no access to the server's memory, only frames.
  CHECK(got.state == reference);
  CHECK(got.resume_from == 500);
}

TEST_CASE("the resume position is the next message, not the last one included",
          "[integration][glimpse]") {
  const auto reference = a_book();
  venue::glimpse_service service{kSession};
  const auto got = consume(served(service, reference, /*next_sequence=*/1'000));

  // Off by one either way looks like a working snapshot on a quiet feed and corrupts a book on a busy one, so it
  // is asserted rather than assumed. 1,000 means "message 999 is in the state; take 1,000 from the live feed".
  CHECK(got.resume_from == 1'000);
}

TEST_CASE("begin comes before any state, so a wrong service is caught before it is believed",
          "[integration][glimpse]") {
  const auto reference = a_book();
  venue::glimpse_service service{kSession};
  const auto stream = served(service, reference, 42);

  // The first Sequenced Data Packet in the stream must be the begin marker. A client that applied levels before
  // confirming what it had connected to would have a book from another session and no way to tell.
  soup::stream_cursor cursor;
  std::size_t at_byte = 0;
  bool first_sequenced_was_begin = false;
  while (at_byte < stream.size()) {
    const dfr::packet_view rest{stream.data() + at_byte, stream.size() - at_byte};
    soup::sequenced_packet next;
    REQUIRE(cursor.next(rest).get(next) == dfr::error::ok);
    at_byte += next.frame.frame_size;
    if (next.frame.type == soup::packet_type::login_accepted) {
      soup::login_accepted accepted;
      REQUIRE(soup::decode_login_accepted(next.frame.payload).get(accepted) == dfr::error::ok);
      cursor.accept_login(accepted.next_sequence);
      continue;
    }
    if (next.frame.type == soup::packet_type::sequenced_data) {
      glimpse::begin_snapshot begin;
      first_sequenced_was_begin =
          glimpse::decode_begin(next.frame.payload).get(begin) == dfr::error::ok;
      break;
    }
  }
  CHECK(first_sequenced_was_begin);
}

TEST_CASE("an empty book serves a valid, empty snapshot", "[integration][glimpse]") {
  const test_book empty;
  venue::glimpse_service service{kSession};
  const auto got = consume(served(service, empty, 7));

  // The case a snapshot service is most likely to have been written without: a client joining before the market
  // opens. It must get a complete session with no levels, not a truncated one.
  CHECK(got.saw_begin);
  CHECK(got.saw_end);
  CHECK(got.session_ended);
  CHECK(got.levels == 0);
  CHECK(got.state.bids().empty());
  CHECK(got.resume_from == 7);
}

TEST_CASE("a snapshot valid as of nothing is refused rather than served",
          "[integration][glimpse]") {
  const auto reference = a_book();
  venue::glimpse_service service{kSession};
  const auto nothing = [](dfr::packet_view) {};
  // Sequence numbering starts at one, so zero means the caller never set it. Serving it would give a client a
  // book plus an instruction to resume from the beginning of the day.
  CHECK(service.serve(reference, "ZTEST", 0, nothing).error_code() == dfr::error::invalid_argument);
}

TEST_CASE("an End Of Snapshot carrying zero is refused by the decoder too",
          "[integration][glimpse]") {
  std::array<std::byte, glimpse::kMessageSize> bytes{};
  const dfr::mutable_packet_view out{bytes.data(), bytes.size()};
  // Built by hand rather than through encode_end, because encode_end is not the only thing that could produce
  // these bytes: a truncated or zero-filled frame arriving off a network produces exactly this.
  out.put_u8_at(glimpse::kTypeOffset,
                static_cast<std::uint8_t>(glimpse::message_type::end_snapshot));
  out.put_le32_at(glimpse::kSessionOffset, kSession);
  out.put_le64_at(glimpse::kSequenceOffset, 0);

  glimpse::end_snapshot end;
  CHECK(glimpse::decode_end(dfr::packet_view{bytes.data(), bytes.size()}).get(end) ==
        dfr::error::snapshot_stale);
}

TEST_CASE("a snapshot carries state, not history", "[integration][glimpse]") {
  auto reference = a_book();
  deep::trade_report trade;
  trade.symbol = "ZTEST";
  trade.size = 250;
  trade.at = at(20, 9500);
  reference.observe(trade);
  REQUIRE(reference.trades() == 1);

  venue::glimpse_service service{kSession};
  const auto got = consume(served(service, reference, 100));

  // The levels match and the trade history does not, and that is correct rather than a gap: a snapshot is
  // O(depth), which is the whole reason it can be served to a client that cannot catch up by replay. Anything
  // reconciling volume has to know this, and a service that replayed trades to hide it would be replaying the
  // day.
  CHECK(got.state == reference);
  CHECK(got.state.trades() == 0);
  CHECK(reference.trades() == 1);
}

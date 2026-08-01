// A feed of real DEEP messages, replayed twice: once intact and once through loss and repair.
//
// What this harness is for, and what it is careful not to claim
// ------------------------------------------------------------
// The existing oracle checks a statement about bookkeeping: every sequence delivered exactly once. This one
// checks a statement about content: **the book after recovery equals the book that never lost anything.** That
// is a harder invariant. It fails if recovery delivers the right messages in the wrong order, or applies a
// repair twice, or drops a size-zero deletion: none of which a sequence count can see.
//
// The body of each message is looked up by sequence number rather than carried through the recovery path, and
// that shortcut decides what the test can claim, so it is stated rather than buried. Recovery's responsibility
// is *which* sequences reach the consumer, *how many times*, and *in what order*; it never rewrites a body. So
// the lookup is equivalent to carrying it, and the test still fails if recovery:
//
//   * delivers a sequence twice: the trade count doubles;
//   * delivers out of order: an aggregated book is last-write-wins, so the sizes diverge;
//   * drops one: a level it should have deleted stays, or one it should have created never appears.
//
// What it cannot catch is a recovery path that corrupted a body it forwarded. Nothing here rewrites a body and
// the wire tests cover the decode, but saying so beats implying a stronger claim.
//
// The reference side is not a second implementation: same decoder, same book, fed the packets the venue actually
// sent. So the comparison means "recovery lost nothing", not "two implementations agree".
//
// The finding that changed this harness
// -------------------------------------
// The first version applied messages in the order the client *delivered* them, and the books did not match:
// same 600 messages, same 137 updates per symbol, different book. That is not a defect in recovery. It is the
// consequence of a design decision recovery makes on purpose: **while a hole is open the client keeps delivering
// later messages**, because stalling on a gap turns one loss into an outage. The repair therefore arrives *after*
// messages with higher sequence numbers.
//
// For an aggregated book that is fatal if consumed naively: price levels are last-write-wins, so applying an
// older update after a newer one leaves the wrong size at that price, forever. So a correct consumer of a
// gap-filling feed must **apply in sequence order, not in arrival order**, which the client makes possible by
// numbering everything it hands over, and which nothing warns you about.
//
// This harness therefore buffers out-of-order deliveries and applies them in sequence order, and
// `book_oracle_test.cpp` keeps a test showing the naive order producing a wrong book, because a hazard nobody
// demonstrates is a hazard everybody rediscovers.

#ifndef DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_FEED_HPP
#define DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_FEED_HPP

#include <dfr/book/order_book.hpp>
#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/schedule.hpp>
#include <dfr/chaos/target.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/venue/publisher.hpp>
#include <dfr/wire/deep.hpp>
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/cursor.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dfr_test::oracle {

namespace book = dfr::book;
namespace chaos = dfr::chaos;
namespace deep = dfr::wire::deep;
namespace iex = dfr::wire::iextp;
namespace rec = dfr::recovery;
namespace ven = dfr::venue;

using feed_clock = dfr::manual_clock;
using feed_time = feed_clock::time_point;
using feed_client = rec::client<feed_clock, rec::replay_buffer<32'768, 4096>>;
using oracle_book = book::order_book<32>;

inline constexpr std::uint32_t kFeedSession = 0xB00C;

inline feed_time at_us(std::int64_t micros) {
  return feed_time{} + std::chrono::microseconds{micros};
}

// Four symbols: enough that a message lost for one must not disturb another's book, few enough that a failure
// names something a reader can follow.
// The single symbol the threaded test uses, named so two files agree on it rather than repeating a literal.
inline constexpr std::string_view kTracedSymbolForTest = "ZTEST";

inline constexpr std::string_view kSymbols[]{"ZTEST", "ZIEXT", "ZWWE", "ZAAPL"};

struct feed_packet {
  std::string bytes;
  std::uint64_t first_sequence{0};
  std::uint64_t message_count{0};
};

struct feed {
  std::vector<feed_packet> packets;
  /** Sequence number to the DEEP body the venue put in it. */
  std::map<std::uint64_t, std::string> bodies;
};

struct replay_result {
  std::map<std::string, oracle_book> books;
  std::uint64_t delivered{0};
  std::uint64_t trades{0};
  std::uint64_t traded_shares{0};
  std::uint64_t gaps_opened{0};
  std::uint64_t unfillable{0};
  std::uint64_t injected{0};
  /**
   * Messages `detail::apply` could not decode.
   *
   * A counter rather than a REQUIRE inside apply, because apply runs on a consumer thread in
   * threaded_book_test.cpp and **Catch2's assertion machinery is not thread-safe**. It aborted inside
   * `OutputRedirect::activate`, a failure in the measuring apparatus that looked like a failure in the library,
   * and looked platform-specific because I had run the suite once. Every caller asserts this is zero from the
   * main thread instead.
   */
  std::uint64_t malformed{0};
  bool failed{false};
};

namespace detail {

inline deep::price at_dollars(std::int64_t dollars, std::int64_t ten_thousandths) {
  return deep::price{dollars * deep::kPriceScale + ten_thousandths};
}

// One DEEP message, chosen from the index so the two replays see byte-identical input and a failure is
// reproducible without a seed.
//
// The mix is deliberate: mostly quotes, because that is what a feed is; a deletion every seventeenth, because
// size-zero-removes-the-level is the rule an implementation most often inverts; a trade every eleventh, because
// a duplicate delivery is invisible in an aggregated book and visible in a trade count.
inline std::string message_for(std::size_t index, std::size_t symbols) {
  std::array<std::byte, 64> scratch{};
  const dfr::mutable_packet_view out{scratch.data(), scratch.size()};
  const auto symbol = kSymbols[index % symbols];
  const auto now = static_cast<std::uint64_t>(1'503'705'600'000'000'000ULL + index * 1'000'000ULL);

  std::size_t written = 0;
  if (index % 11 == 10) {
    const auto price = at_dollars(20 + static_cast<std::int64_t>(index % 5), 5000);
    REQUIRE(deep::encode_trade(out, symbol, 100 + static_cast<std::uint32_t>(index % 400), price,
                               index, now)
                .get(written) == dfr::error::ok);
  } else {
    const bool buy = index % 2 == 0;
    // A ladder that walks, so levels are created and superseded rather than one price being rewritten.
    const auto ticks = static_cast<std::int64_t>((index / 4) % 6) * 100;
    const auto price = buy ? at_dollars(20, 8000 + ticks) : at_dollars(20, 9500 + ticks);
    const std::uint32_t size = index % 17 == 16 ? 0  // the deletion
                                                : 100 + static_cast<std::uint32_t>(index % 900);
    REQUIRE(deep::encode_price_level(out, buy, symbol, size, price, now).get(written) ==
            dfr::error::ok);
  }
  return std::string{reinterpret_cast<const char*>(scratch.data()), written};
}

inline ven::publisher_options feed_publisher_options() {
  ven::publisher_options options;
  options.session = kFeedSession;
  options.first_sequence = 1;
  return options;
}

inline rec::client_options feed_client_options() {
  rec::client_options options;
  options.lines = 1;
  return options;
}

// Applies one message to the right book. The only place the two replays share code, on purpose: a difference
// here would show up as both books being wrong in the same way, which the equality would not catch.
//
// No Catch2 macros in here, and that is a requirement rather than a style choice: threaded_book_test.cpp calls this
// from a consumer thread, and a REQUIRE on a second thread reaches into Catch2's result capture and output redirect
// concurrently with the main thread. It aborts: `Assertion !m_redirectActive && "redirect is already active"`,
// with no failing expression, which reads as a library defect and is not one. A decode failure is counted here and
// asserted by the caller on the main thread.
inline void apply(std::map<std::string, oracle_book>& books, dfr::packet_view body,
                  replay_result& into) {
  deep::header head;
  if (deep::decode_header(body).get(head) != dfr::error::ok) {
    ++into.malformed;
    return;
  }
  if (head.type == deep::message_type::price_level_buy ||
      head.type == deep::message_type::price_level_sell) {
    deep::price_level_update update;
    if (deep::decode_price_level(body).get(update) != dfr::error::ok) {
      ++into.malformed;
      return;
    }
    (void)books[std::string{update.symbol}].apply(update);
    return;
  }
  if (head.type == deep::message_type::trade_report) {
    deep::trade_report trade;
    if (deep::decode_trade(body).get(trade) != dfr::error::ok) {
      ++into.malformed;
      return;
    }
    books[std::string{trade.symbol}].observe(trade);
    into.trades += 1;
    into.traded_shares += trade.size;
  }
}

// The fault kinds this test can reason about.
//
// A bit flip rewrites a message body, and the premise here is that recovery does not touch bodies, so
// including it would be testing the injector rather than the client. Sequence and session rewrites are excluded
// for the same reason the sequence oracle excludes them: they make the stream a different stream.
inline chaos::op_mask book_safe_faults() {
  chaos::op_mask mask;
  mask.disable(chaos::fault_op::flip_bit);
  mask.disable(chaos::fault_op::rewrite_sequence);
  mask.disable(chaos::fault_op::rewrite_session);
  return mask;
}

}  // namespace detail

// Publishes `messages` DEEP messages and keeps both the packets and the bodies.
inline feed publish_feed(std::size_t messages, std::size_t symbols) {
  ven::iextp_publisher<feed_clock> publisher{detail::feed_publisher_options()};
  feed out;

  const auto capture = [&](dfr::packet_view packet) {
    iex::header header;
    REQUIRE(iex::decode_header(packet).get(header) == dfr::error::ok);
    out.packets.push_back(
        feed_packet{.bytes = std::string{reinterpret_cast<const char*>(packet.data()),
                                         packet.size()},
                    .first_sequence = header.first_sequence,
                    .message_count = header.message_count});
  };

  std::int64_t now = 0;
  std::uint64_t sequence = 1;
  for (std::size_t i = 0; i < messages; ++i) {
    const std::string body = detail::message_for(i, symbols);
    out.bodies[sequence] = body;
    ++sequence;
    now += 5;
    REQUIRE(publisher.submit(dfr::packet_view{body.data(), body.size()}, at_us(now), capture)
                .has_value());
    // Two to four messages a packet, so a hole is not always one message wide.
    if (i % 3 == 2) {
      REQUIRE(publisher.flush(at_us(now), capture).has_value());
    }
  }
  REQUIRE(publisher.flush(at_us(now), capture).has_value());
  return out;
}

}  // namespace dfr_test::oracle

#endif  // DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_FEED_HPP

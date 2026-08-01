// A feed that means something, so a trace can carry a book.
//
// The traces the viewer draws used to carry `"msg-" + i` as every message body. That was enough to prove a
// statement about bookkeeping(every sequence delivered exactly once) and it made the strongest statement the
// project can make unshowable: **the book after loss and repair equals the book that lost nothing.** That
// invariant lives in `book_oracle_test.cpp` and a visitor cannot see it, because the drawing has no book in it.
//
// So the trace tool publishes real DEEP messages, and the trace carries the resulting top of book at every step.
// Same rule as always: the C++ decides, the viewer draws, nothing is recomputed in TypeScript. A viewer that
// applied price levels itself would be a second implementation of an order book written by somebody reading the
// first, which is the rule this file exists to keep rather than break.
//
// Why a deterministic ladder rather than random quotes
// ---------------------------------------------------
// Every field comes from the message index. Two reasons, and the second is the one that matters: a trace has to
// reproduce byte for byte from its seed, and the market data is not the thing under test. A random walk would make
// every regenerated trace a diff and hide the behavioural changes the committed traces exist to reveal.
//
// The shape is chosen to exercise a book rather than to look like a market: levels created, levels superseded,
// levels *deleted*(the size-zero update that an implementation most often gets backwards) and trades, which are
// the one thing a duplicate delivery shows up in.

#ifndef DFR_TOOLS_SUPPORT_TRACED_MARKET_HPP
#define DFR_TOOLS_SUPPORT_TRACED_MARKET_HPP

#include <dfr/book/order_book.hpp>
#include <dfr/wire/deep.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dfr_tools {

namespace book = dfr::book;
namespace deep = dfr::wire::deep;

// One symbol. The trace draws one book, and four symbols would mean the drawing either picks one arbitrarily or
// shows four ladders nobody can read at that size.
inline constexpr std::string_view kTracedSymbol = "ZTEST";

// Sixteen levels a side is more depth than the drawing can show and less than a real feed publishes. Chosen so the
// book is never full: a refused level would be a property of this constant rather than of the run.
using traced_book = book::order_book<16>;

inline deep::price at_dollars(std::int64_t dollars, std::int64_t ten_thousandths) {
  return deep::price{dollars * deep::kPriceScale + ten_thousandths};
}

// The DEEP message for message `index`, as bytes.
//
// A ladder that walks: the bid climbs and the ask follows, so levels are created and superseded rather than one
// price being rewritten forever. Every seventeenth is a deletion and every eleventh a trade.
inline std::string traced_message(std::size_t index) {
  std::array<std::byte, 64> scratch{};
  const dfr::mutable_packet_view out{scratch.data(), scratch.size()};
  // A fixed epoch rather than a clock: the trace's timestamps come from its own manual clock, and a second source
  // of time in the same file would be two answers to one question.
  const auto when = 1'503'705'600'000'000'000ULL + static_cast<std::uint64_t>(index) * 1'000'000ULL;

  std::size_t written = 0;
  const auto ticks = static_cast<std::int64_t>((index / 4) % 8) * 100;

  if (index % 11 == 10) {
    // A trade at the current offer, which is what an aggressive buy does.
    const auto price = at_dollars(20, 9500 + ticks);
    if (deep::encode_trade(out, kTracedSymbol, 100 + static_cast<std::uint32_t>(index % 400), price,
                           index, when)
            .get(written) != dfr::error::ok) {
      return {};
    }
    return std::string{reinterpret_cast<const char*>(scratch.data()), written};
  }

  const bool buy = index % 2 == 0;
  const auto price = buy ? at_dollars(20, 8000 + ticks) : at_dollars(20, 9500 + ticks);
  // The deletion. Size zero removes the level, which is the rule a book implementation most often inverts, so the
  // trace exercises it rather than describing it.
  const std::uint32_t size =
      index % 17 == 16 ? 0 : 100 + static_cast<std::uint32_t>((index * 37) % 900);
  if (deep::encode_price_level(out, buy, kTracedSymbol, size, price, when).get(written) !=
      dfr::error::ok) {
    return {};
  }
  return std::string{reinterpret_cast<const char*>(scratch.data()), written};
}

// Applies one message to a book, and says whether anything changed.
//
// Returns false for a message that is not a quote(a trade, or bytes that do not decode) so a caller counting
// book updates does not count them. A trade does not move an aggregated book: the size reduction arrives as its
// own price level update, and a book that also decremented here would double-count every fill.
inline bool apply_to_book(traced_book& into, dfr::packet_view body) {
  deep::header head;
  if (deep::decode_header(body).get(head) != dfr::error::ok) {
    return false;
  }
  if (head.type == deep::message_type::price_level_buy ||
      head.type == deep::message_type::price_level_sell) {
    deep::price_level_update update;
    if (deep::decode_price_level(body).get(update) != dfr::error::ok) {
      return false;
    }
    return into.apply(update).has_value();
  }
  if (head.type == deep::message_type::trade_report) {
    deep::trade_report trade;
    if (deep::decode_trade(body).get(trade) != dfr::error::ok) {
      return false;
    }
    into.observe(trade);
  }
  return false;
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_TRACED_MARKET_HPP

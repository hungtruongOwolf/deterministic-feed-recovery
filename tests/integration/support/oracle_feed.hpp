// A feed of real DEEP messages, replayed twice: once intact and once through loss and repair.
//
// What this harness is for, and what it is careful not to claim
// ------------------------------------------------------------
// The existing oracle checks a statement about bookkeeping — every sequence delivered exactly once. This one
// checks a statement about content: **the book after recovery equals the book that never lost anything.** That
// is a harder invariant. It fails if recovery delivers the right messages in the wrong order, or applies a
// repair twice, or drops a size-zero deletion — none of which a sequence count can see.
//
// The body of each message is looked up by sequence number rather than carried through the recovery path, and
// that shortcut decides what the test can claim, so it is stated rather than buried. Recovery's responsibility
// is *which* sequences reach the consumer, *how many times*, and *in what order*; it never rewrites a body. So
// the lookup is equivalent to carrying it, and the test still fails if recovery:
//
//   * delivers a sequence twice — the trade count doubles;
//   * delivers out of order — an aggregated book is last-write-wins, so the sizes diverge;
//   * drops one — a level it should have deleted stays, or one it should have created never appears.
//
// What it cannot catch is a recovery path that corrupted a body it forwarded. Nothing here rewrites a body and
// the wire tests cover the decode, but saying so beats implying a stronger claim.
//
// The reference side is not a second implementation: same decoder, same book, fed the packets the venue actually
// sent. So the comparison means "recovery lost nothing", not "two implementations agree".
//
// The finding that changed this harness
// -------------------------------------
// The first version applied messages in the order the client *delivered* them, and the books did not match —
// same 600 messages, same 137 updates per symbol, different book. That is not a defect in recovery. It is the
// consequence of a design decision recovery makes on purpose: **while a hole is open the client keeps delivering
// later messages**, because stalling on a gap turns one loss into an outage. The repair therefore arrives *after*
// messages with higher sequence numbers.
//
// For an aggregated book that is fatal if consumed naively: price levels are last-write-wins, so applying an
// older update after a newer one leaves the wrong size at that price, forever. So a correct consumer of a
// gap-filling feed must **apply in sequence order, not in arrival order** — which the client makes possible by
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
inline void apply(std::map<std::string, oracle_book>& books, dfr::packet_view body,
                  replay_result& into) {
  deep::header head;
  if (deep::decode_header(body).get(head) != dfr::error::ok) {
    return;
  }
  if (head.type == deep::message_type::price_level_buy ||
      head.type == deep::message_type::price_level_sell) {
    deep::price_level_update update;
    REQUIRE(deep::decode_price_level(body).get(update) == dfr::error::ok);
    (void)books[std::string{update.symbol}].apply(update);
    return;
  }
  if (head.type == deep::message_type::trade_report) {
    deep::trade_report trade;
    REQUIRE(deep::decode_trade(body).get(trade) == dfr::error::ok);
    books[std::string{trade.symbol}].observe(trade);
    into.trades += 1;
    into.traded_shares += trade.size;
  }
}

// The fault kinds this test can reason about.
//
// A bit flip rewrites a message body, and the premise here is that recovery does not touch bodies — so
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

// The reference: every packet, in order, nothing lost.
inline replay_result replay_clean(const feed& source) {
  replay_result out;
  for (const auto& packet : source.packets) {
    for (std::uint64_t s = packet.first_sequence;
         s < packet.first_sequence + packet.message_count; ++s) {
      const auto found = source.bodies.find(s);
      REQUIRE(found != source.bodies.end());
      detail::apply(out.books,
                    dfr::packet_view{found->second.data(), found->second.size()}, out);
      ++out.delivered;
    }
  }
  return out;
}

// The subject: the same packets through the injector and the recovery client.
//
// Retransmits are answered from the published stream, which is a harness with the whole day in memory rather
// than a facility that forgets — that path is covered in venue_recovery_test.cpp. What is under test here is
// what the *client* delivered, so the server is deliberately not the variable.
inline replay_result replay_recovered(const feed& source, std::uint64_t seed, std::uint32_t faults,
                                      bool glimpse = false,
                                      bool apply_in_arrival_order = false) {
  replay_result out;
  feed_client client{detail::feed_client_options()};
  std::int64_t now = 0;

  chaos::schedule plan;
  if (faults > 0) {
    dfr::prng rng{seed};
    chaos::schedule_options options;
    options.max_faults = faults;
    // The same restriction the sequence oracle uses: the kinds whose consequence a harness can reason about
    // without reimplementing the injector. A bit flip rewrites a body, and this test's whole premise is that
    // recovery does not — so including it would be testing the injector, not the client.
    options.permitted = detail::book_safe_faults();
    REQUIRE(chaos::schedule::generate(rng, options, source.packets.size()).get(plan) ==
            dfr::error::ok);
  }
  chaos::injector<chaos::iextp_target> injector{plan};

  // Delivered sequences, held until they can be applied in order.
  //
  // `next_to_apply` is the sequence the book is waiting for; anything above it waits in `pending` until the hole
  // below it closes. That is the whole of what a consumer of this feed has to do, and it is three lines.
  std::map<std::uint64_t, bool> pending;
  std::uint64_t next_to_apply = 1;

  const auto drain_in_order = [&]() {
    while (pending.erase(next_to_apply) > 0 || source.bodies.count(next_to_apply) == 0) {
      const auto found = source.bodies.find(next_to_apply);
      if (found != source.bodies.end()) {
        detail::apply(out.books, dfr::packet_view{found->second.data(), found->second.size()}, out);
        ++out.delivered;
      } else if (next_to_apply > source.bodies.rbegin()->first) {
        break;  // past the end of what the venue published
      }
      ++next_to_apply;
    }
  };

  const auto deliver = [&](rec::sequence_range range) {
    for (std::uint64_t s = range.first; s < range.end; ++s) {
      if (apply_in_arrival_order) {
        const auto found = source.bodies.find(s);
        if (found == source.bodies.end()) {
          continue;
        }
        detail::apply(out.books, dfr::packet_view{found->second.data(), found->second.size()}, out);
        ++out.delivered;
        continue;
      }
      pending[s] = true;
    }
    if (!apply_in_arrival_order) {
      drain_in_order();
    }
  };

  const auto offer = [&](dfr::packet_view packet) {
    iex::header header;
    if (iex::decode_header(packet).get(header) != dfr::error::ok) {
      return;
    }
    // Framing is checked by walking the messages: a payload whose blocks do not add up fails here exactly as
    // it would in a receiver, and there is no separate validator to call.
    auto walk = iex::message_cursor::over(packet);
    if (!walk) {
      return;
    }
    rec::ingest_report report;
    if (client
            .on_packet(0, header.session, header.first_sequence, header.message_count, 0,
                       at_us(now))
            .get(report) != dfr::error::ok) {
      out.failed = true;
      return;
    }
    if (!report.gap_opened.empty()) {
      ++out.gaps_opened;
    }
    if (report.held_for_replay) {
      for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
        const auto found = source.bodies.find(s);
        const char filler = 'm';
        const auto body = found == source.bodies.end()
                              ? dfr::packet_view{&filler, 1}
                              : dfr::packet_view{found->second.data(), found->second.size()};
        if (!client.buffer_message(s, body)) {
          out.failed = true;
          return;
        }
      }
      return;
    }
    // Two independent things, and conflating them was a real bug here: a retransmit packet can repair a hole
    // while delivering nothing new, so `delivered()` is false and the repair still has to reach the book. The
    // first version guarded both with `delivered()` and lost every repair.
    for (const auto& repaired : client.last_recovered().ranges()) {
      deliver(repaired);
    }
    if (report.delivered()) {
      deliver(report.accepted);
    }
  };

  const auto answer = [&]() {
    // Bounded, so a client that asks for the same range forever fails the test rather than hanging it.
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto decision = client.poll(at_us(now));
      if (decision.what != rec::client_action::send_retransmit_request) {
        return;
      }
      if (glimpse) {
        REQUIRE(client
                    .on_retransmit_refused(decision.range,
                                           dfr::error::retransmit_window_exceeded)
                    .has_value());
        return;
      }
      for (const auto& candidate : source.packets) {
        const rec::sequence_range carried{
            .first = candidate.first_sequence,
            .end = candidate.first_sequence + candidate.message_count};
        if (carried.overlaps(decision.range)) {
          offer(dfr::packet_view{candidate.bytes.data(), candidate.bytes.size()});
        }
      }
    }
  };

  const auto emit = [&](const chaos::emission& e) {
    now += 50;  // past the settle delay, so a hole becomes a request promptly
    offer(e.packet);
    answer();
  };

  for (std::uint64_t i = 0; i < source.packets.size(); ++i) {
    const auto& packet = source.packets[i];
    REQUIRE(injector
                .offer(dfr::packet_view{packet.bytes.data(), packet.bytes.size()}, i, emit)
                .has_value());
  }
  REQUIRE(injector.flush(emit).has_value());
  for (int settle = 0; settle < 32; ++settle) {
    now += 100;
    answer();
  }

  out.unfillable = client.total_missing();
  out.injected = injector.stats().dropped;
  if (client.state() == rec::client_state::failed) {
    out.failed = true;
  }
  return out;
}

}  // namespace dfr_test::oracle

#endif  // DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_FEED_HPP

// Serving a Glimpse snapshot and consuming it, in one place, so the tool and the browser cannot diverge.
//
// The consumer here is a real one: a `stream_cursor` framing bytes and an `order_book` applying decoded messages. It
// has no access to the service's memory and is told nothing about what was sent. That is the whole claim — a client
// with nothing but frames ends up with the venue's book — and a harness that took a shortcut anywhere would be
// checking that the service can describe itself.

#ifndef DFR_TOOLS_SUPPORT_GLIMPSE_RUN_HPP
#define DFR_TOOLS_SUPPORT_GLIMPSE_RUN_HPP

#include "support/glimpse_trace.hpp"
#include "support/traced_market.hpp"

#include <dfr/venue/glimpse_service.hpp>
#include <dfr/wire/glimpse.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace dfr_tools {

namespace soup = dfr::wire::soupbintcp;
namespace glimpse = dfr::wire::glimpse;

struct glimpse_recording {
  traced_book venue;
  std::vector<glimpse_step> steps;
  std::uint64_t resume_from{0};
  bool matched{false};
};

inline constexpr std::uint32_t kGlimpseSession = 0xBEEF;

// A venue book with `levels` a side, built from the same deterministic market the traces use.
//
// Deliberately the same generator: a snapshot of a book nobody has seen before is a snapshot of an arbitrary thing,
// and a reader who has just watched the film should recognise the prices.
inline traced_book glimpse_book(std::size_t levels) {
  traced_book out;
  for (std::size_t i = 0; i < levels; ++i) {
    const auto ticks = static_cast<std::int64_t>(i) * 100;
    dfr::wire::deep::price_level_update bid;
    bid.buy = true;
    bid.symbol = kTracedSymbol;
    bid.level = at_dollars(20, 8700 - ticks);
    bid.size = static_cast<std::uint32_t>(100 + i * 130);
    bid.head.type = dfr::wire::deep::message_type::price_level_buy;
    (void)out.apply(bid);

    dfr::wire::deep::price_level_update ask;
    ask.buy = false;
    ask.symbol = kTracedSymbol;
    ask.level = at_dollars(20, 9500 + ticks);
    ask.size = static_cast<std::uint32_t>(150 + i * 90);
    ask.head.type = dfr::wire::deep::message_type::price_level_sell;
    (void)out.apply(ask);
  }
  return out;
}

// Serves one snapshot and records what a client rebuilds from it, frame by frame.
inline glimpse_recording run_glimpse_session(std::size_t levels, std::uint64_t resume_from) {
  glimpse_recording out;
  out.venue = glimpse_book(levels);
  out.resume_from = resume_from;

  dfr::venue::glimpse_service service{kGlimpseSession};
  std::string stream;
  const auto capture = [&](dfr::packet_view frame) {
    stream.append(reinterpret_cast<const char*>(frame.data()), frame.size());
  };
  if (!service.serve(out.venue, kTracedSymbol, resume_from, capture)) {
    return out;
  }

  // The client. Nothing but the bytes.
  traced_book client;
  soup::stream_cursor cursor;
  std::size_t at = 0;
  std::size_t step = 0;
  std::uint64_t resume = 0;

  const auto snapshot_of = [&](char type, std::string_view name, std::string detail) {
    glimpse_step s;
    s.step = step++;
    s.type = type;
    s.name = name;
    s.detail = std::move(detail);
    s.bid = client.bids().best().at.raw();
    s.bid_size = client.bids().best().size;
    s.ask = client.asks().best().at.raw();
    s.ask_size = client.asks().best().size;
    s.bid_levels = static_cast<std::uint16_t>(client.bids().size());
    s.ask_levels = static_cast<std::uint16_t>(client.asks().size());
    s.resume_from = resume;
    s.matches = client == out.venue;
    out.steps.push_back(std::move(s));
  };

  while (at < stream.size()) {
    const dfr::packet_view rest{stream.data() + at, stream.size() - at};
    soup::sequenced_packet next;
    if (cursor.next(rest).get(next) != dfr::error::ok) {
      break;
    }
    at += next.frame.frame_size;

    if (next.frame.type == soup::packet_type::login_accepted) {
      soup::login_accepted accepted;
      if (soup::decode_login_accepted(next.frame.payload).get(accepted) == dfr::error::ok) {
        cursor.accept_login(accepted.next_sequence);
      }
      snapshot_of('A', "Login Accepted", "the snapshot stream is its own numbered stream");
      continue;
    }
    if (next.frame.type == soup::packet_type::end_of_session) {
      snapshot_of('Z', "End Of Session", "complete, not truncated by a dropped connection");
      continue;
    }
    if (next.frame.type != soup::packet_type::sequenced_data || next.frame.payload.empty()) {
      continue;
    }

    const auto body = next.frame.payload;
    if (glimpse::is_glimpse_type(body.u8_at(0))) {
      glimpse::begin_snapshot begin;
      if (glimpse::decode_begin(body).get(begin) == dfr::error::ok) {
        snapshot_of('g', "Begin Snapshot", "before any state, so a wrong service is caught first");
        continue;
      }
      glimpse::end_snapshot end;
      if (glimpse::decode_end(body).get(end) == dfr::error::ok) {
        resume = end.next_sequence;
        char detail[80];
        std::snprintf(detail, sizeof detail, "resume the live feed at %llu",
                      static_cast<unsigned long long>(resume));
        snapshot_of('G', "End Of Snapshot", detail);
      }
      continue;
    }

    dfr::wire::deep::price_level_update update;
    if (dfr::wire::deep::decode_price_level(body).get(update) != dfr::error::ok) {
      continue;
    }
    (void)client.apply(update);
    char detail[96];
    std::snprintf(detail, sizeof detail, "%s %u @ %lld.%04lld", update.buy ? "bid" : "ask",
                  update.size, static_cast<long long>(update.level.dollars()),
                  static_cast<long long>(update.level.ten_thousandths()));
    snapshot_of('S', "Price Level", detail);
  }

  out.matched = client == out.venue;
  return out;
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_GLIMPSE_RUN_HPP

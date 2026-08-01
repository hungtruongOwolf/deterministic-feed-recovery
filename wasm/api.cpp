// The library, compiled for a browser.
//
// Why this exists
// ---------------
// The page used to draw traces that had been generated once and committed. Everything on it was correct and
// none of it was *yours*: you could not change the seed, take a line away, or ask for more faults and see
// what happened. A viewer over fixtures is a screenshot with extra steps.
//
// This library is unusually well suited to running in a browser, and not by accident. It is header-only, it
// reads no clock, it opens no sockets, it allocates nothing after start-up, and every run is a function of
// its seed. Those were determinism constraints, adopted so a failing run could be replayed from a number,
// and they are exactly the constraints that make code portable to a sandbox with no operating system.
//
// So the browser runs the same code the tests run. Not a reimplementation of it in TypeScript.
//
// One writer, two hosts
// ---------------------
// The output goes through dfr_tools::write_header/write_event/write_summary, the same functions
// tools/trace.cpp uses: into Emscripten's in-memory filesystem, which is then read back as a string. A
// second formatter for the browser would have been a second format, and the two would have drifted. As it
// stands a trace produced in a browser is byte-identical to one produced in a terminal, and
// scripts/check-wasm.sh asserts exactly that.
//
// Nothing here is exposed but two functions returning JSONL. The interface is a string, because the
// viewer's rule is that it draws a trace file and knows nothing about the state machine that produced it.
// Handing JavaScript a live object graph would have broken that rule under the guise of efficiency.

#include "support/glimpse_run.hpp"
#include "support/session_trace.hpp"
#include "support/trace_writer.hpp"
#include "support/traced_drivers.hpp"

#include <dfr/venue/order_session.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ouch = dfr::wire::ouch;
namespace soup = dfr::wire::soupbintcp;
namespace venue = dfr::venue;

namespace {

// The returned string, kept alive between the call and JavaScript reading it.
//
// One buffer rather than a malloc the caller must free: the alternative is an ownership contract across a
// language boundary, which is a leak waiting for the one path that forgets. The next call overwrites it,
// which is stated here and is all a caller needs to know.
std::string g_result;

// Runs `write` against a file in Emscripten's memory filesystem and returns what it wrote.
//
// Going through a FILE* looks indirect and is the point: it lets the browser use the writers the native
// tool uses, unmodified. A version taking a std::string would have meant two writers.
template <typename Write>
const char* rendered(Write&& write) {
  const char* path = "/dfr-output.jsonl";
  std::FILE* out = std::fopen(path, "wb");
  if (out == nullptr) {
    g_result = "{\"kind\":\"error\",\"message\":\"could not open the output buffer\"}\n";
    return g_result.c_str();
  }
  write(out);
  std::fclose(out);

  std::FILE* in = std::fopen(path, "rb");
  if (in == nullptr) {
    g_result = "{\"kind\":\"error\",\"message\":\"could not read the output buffer back\"}\n";
    return g_result.c_str();
  }
  g_result.clear();
  char chunk[8192];
  std::size_t read = 0;
  while ((read = std::fread(chunk, 1, sizeof chunk, in)) > 0) {
    g_result.append(chunk, read);
  }
  std::fclose(in);
  std::remove(path);
  return g_result.c_str();
}

// Bounds, because a browser control is an untrusted caller.
//
// The library asserts its own preconditions and an assertion in WebAssembly aborts the module for the rest
// of the page's life. So the numbers are clamped here, at the boundary, which is what core/error.hpp means
// by reserving `invalid_argument` for edges outside our control.
template <typename T>
T clamped(T value, T low, T high) {
  return value < low ? low : (value > high ? high : value);
}

}  // namespace

extern "C" {

// One recovery run, as JSONL. The same output as `tools/trace`, for the same arguments.
EMSCRIPTEN_KEEPALIVE
const char* dfr_run_trace(double seed, int messages, int faults, int lines, int glimpse,
                          int staleness) {
  dfr_tools::run_options run;
  run.seed = static_cast<std::uint64_t>(seed < 0 ? 0 : seed);
  run.messages = static_cast<std::size_t>(clamped(messages, 20, 1200));
  run.faults = static_cast<std::uint32_t>(clamped(faults, 0, 60));
  run.lines = static_cast<std::size_t>(clamped(lines, 1, 2));
  run.mode = glimpse != 0 ? dfr_tools::run_mode::glimpse : dfr_tools::run_mode::recovering;
  run.staleness_messages = static_cast<std::uint64_t>(clamped(staleness, 0, 200));
  if (run.mode == dfr_tools::run_mode::glimpse && run.staleness_messages == 0) {
    run.staleness_messages = 20;  // enough to land in the gap; the same default the tool uses
  }

  dfr_tools::trace_recorder recorder;
  std::int64_t clock_us = 0;
  // The message bodies, so the trace can carry the book they build. See support/traced_market.hpp.
  std::map<std::uint64_t, std::string> bodies;
  const auto stream = dfr_tools::publish_stream(run.messages, recorder, clock_us, &bodies);
  if (stream.empty()) {
    g_result = "{\"kind\":\"error\",\"message\":\"the publisher produced nothing\"}\n";
    return g_result.c_str();
  }
  dfr_tools::run_summary summary = dfr_tools::run_traced(run, stream, recorder, &bodies);
  {
    // The loss-free book, so the page can state the invariant rather than show a quote a reader must interpret.
    dfr_tools::traced_book reference;
    for (const auto& [sequence, body] : bodies) {
      (void)sequence;
      (void)dfr_tools::apply_to_book(reference, dfr::packet_view{body.data(), body.size()});
    }
    summary.reference_bid = reference.bids().best().at.raw();
    summary.reference_ask = reference.asks().best().at.raw();
    summary.reference_traded = reference.traded_shares();
  }

  return rendered([&](std::FILE* out) {
    dfr_tools::write_header(out, run, summary, stream.size());
    for (const auto& event : recorder.events()) {
      dfr_tools::write_event(out, event);
    }
    dfr_tools::write_summary(out, summary, recorder);
  });
}

// One order-entry session, as JSONL. The same output as `tools/session --trace`.
//
// The script is duplicated from tools/session.cpp rather than shared, and that is a deliberate line: the
// tool's script is bound up with printing to a terminal, and pulling it apart to share fifteen lines would
// have coupled two callers to one another's argument handling. The *writers* are shared, which is where a
// divergence would actually matter, and check-wasm.sh compares the two outputs byte for byte.
EMSCRIPTEN_KEEPALIVE
const char* dfr_run_session(int orders, int fill, int cancel) {
  const int order_count = clamped(orders, 1, 24);
  const int fill_shares = clamped(fill, 0, static_cast<int>(dfr_tools::shares_for(0)));
  const bool do_cancel = cancel != 0;

  dfr::manual_clock clock;
  const venue::order_session_options session_options{};
  venue::order_session<dfr::manual_clock> host{session_options, venue::order_entry_options{}};
  soup::stream_cursor cursor;

  struct step {
    std::string from;
    char type = ' ';
    std::string name;
    std::string detail;
    std::uint64_t sequence = 0;
    std::uint64_t server_next = 0;
    std::uint64_t client_next = 0;
    std::size_t live_orders = 0;
    std::uint64_t shares_open = 0;
    std::string phase;
  };
  std::vector<step> journal;
  std::vector<std::string> tokens;

  const auto token_of = [](std::string_view text) {
    ouch::order_token out;
    (void)ouch::order_token::from_text(text).get(out);
    return out;
  };

  const auto shares_open = [&]() {
    std::uint64_t total = 0;
    for (const auto& text : tokens) {
      if (const auto* order = host.orders().find(token_of(text)); order != nullptr) {
        total += order->shares_open();
      }
    }
    return total;
  };

  const auto note = [&](std::string_view from, char type, std::string name, std::string detail,
                        std::uint64_t sequence) {
    journal.push_back(step{.from = std::string{from},
                           .type = type,
                           .name = std::move(name),
                           .detail = std::move(detail),
                           .sequence = sequence,
                           .server_next = host.next_sequence(),
                           .client_next = cursor.next_sequence(),
                           .live_orders = host.orders().live_orders(),
                           .shares_open = shares_open(),
                           .phase = std::string{venue::name_of(host.phase())}});
  };

  const auto encode_into = [](auto&& encode, std::size_t capacity) {
    std::vector<std::byte> scratch(capacity);
    std::size_t size = 0;
    if (encode(dfr::mutable_packet_view{scratch.data(), scratch.size()}).get(size) !=
        dfr::error::ok) {
      return std::string{};
    }
    return std::string{reinterpret_cast<const char*>(scratch.data()), size};
  };

  const auto framed = [&](soup::packet_type type, std::string_view payload) {
    return encode_into(
        [&](dfr::mutable_packet_view out) {
          return soup::encode_packet(out, type, dfr::packet_view{payload.data(), payload.size()});
        },
        soup::kMaxPacketBytes);
  };

  const auto emit = [&](dfr::packet_view frame) {
    soup::sequenced_packet next;
    if (cursor.next(frame).get(next) != dfr::error::ok) {
      note("server", '?', "undecodable", "", 0);
      return;
    }
    switch (next.frame.type) {
      case soup::packet_type::login_accepted: {
        soup::login_accepted accepted;
        if (soup::decode_login_accepted(next.frame.payload).get(accepted) != dfr::error::ok) {
          return;
        }
        cursor.accept_login(accepted.next_sequence);
        char detail[96];
        std::snprintf(detail, sizeof detail, "session=%.*s next sequence=%llu",
                      static_cast<int>(accepted.session.size()), accepted.session.data(),
                      static_cast<unsigned long long>(accepted.next_sequence));
        note("server", 'A', "Login Accepted", detail, 0);
        return;
      }
      case soup::packet_type::sequenced_data: {
        const auto payload = next.frame.payload;
        char detail[160];
        switch (static_cast<ouch::outbound_type>(payload.u8_at(0))) {
          case ouch::outbound_type::accepted: {
            ouch::accepted decoded;
            if (ouch::decode_accepted(payload).get(decoded) != dfr::error::ok) {
              break;
            }
            const auto text = decoded.token.text();
            std::snprintf(detail, sizeof detail, "token=%.*s shares=%u state=%c",
                          static_cast<int>(text.size()), text.data(), decoded.shares_accepted,
                          static_cast<char>(decoded.order.state));
            note("server", 'S', "Accepted", detail, next.sequence);
            return;
          }
          case ouch::outbound_type::executed: {
            ouch::executed decoded;
            if (ouch::decode_executed(payload).get(decoded) != dfr::error::ok) {
              break;
            }
            const auto text = decoded.token.text();
            std::snprintf(detail, sizeof detail, "token=%.*s shares=%u match=%llu",
                          static_cast<int>(text.size()), text.data(), decoded.shares_this_fill,
                          static_cast<unsigned long long>(decoded.match_number));
            note("server", 'S', "Executed", detail, next.sequence);
            return;
          }
          case ouch::outbound_type::canceled: {
            ouch::canceled decoded;
            if (ouch::decode_canceled(payload).get(decoded) != dfr::error::ok) {
              break;
            }
            const auto text = decoded.token.text();
            const auto why = ouch::name_of_cancel_reason(decoded.reason);
            std::snprintf(detail, sizeof detail, "token=%.*s removed=%u reason=%.*s",
                          static_cast<int>(text.size()), text.data(), decoded.shares_decremented,
                          static_cast<int>(why.size()), why.data());
            note("server", 'S', "Canceled", detail, next.sequence);
            return;
          }
          default:
            break;
        }
        std::snprintf(detail, sizeof detail, "%zu bytes", payload.size());
        note("server", 'S', std::string{"type '"} + static_cast<char>(payload.u8_at(0)) + "'",
             detail, next.sequence);
        return;
      }
      case soup::packet_type::server_heartbeat:
        note("server", 'H', "Server Heartbeat", "", 0);
        return;
      case soup::packet_type::end_of_session:
        note("server", 'Z', "End Of Session", "", 0);
        return;
      default:
        note("server", static_cast<char>(next.frame.type), "unexpected", "", 0);
        return;
    }
  };

  const auto offer = [&](std::string_view bytes) {
    std::size_t taken = 0;
    (void)host.offer(dfr::packet_view{bytes.data(), bytes.size()}, clock.now(), emit).get(taken);
  };

  note("client", 'L', "Login Request", "user=DFRUSR", 0);
  offer(encode_into(
      [](dfr::mutable_packet_view out) {
        return soup::encode_login_request(out, "DFRUSR", "DFRPASS", "", 0);
      },
      soup::kMaxPacketBytes));

  for (int i = 0; i < order_count; ++i) {
    char text[16];
    std::snprintf(text, sizeof text, "ORDER%04d", i + 1);
    tokens.emplace_back(text);
    char detail[96];
    std::snprintf(detail, sizeof detail, "token=%s %u %.*s %s @%u.00", text,
                  dfr_tools::shares_for(i),
                  static_cast<int>(dfr_tools::symbol_for(i).size()), dfr_tools::symbol_for(i).data(),
                  i % 3 == 2 ? "sell" : "buy", dfr_tools::price_for(i));
    note("client", 'U', "Enter Order", detail, 0);

    ouch::enter_order order;
    order.token = token_of(text);
    order.order_side = i % 3 == 2 ? ouch::side::sell : ouch::side::buy;
    order.shares = dfr_tools::shares_for(i);
    order.stock = dfr_tools::symbol_for(i);
    (void)ouch::price::from_dollars_and_ten_thousandths(dfr_tools::price_for(i), 0)
        .get(order.limit);
    order.time_in_force = ouch::kSystemHours;
    order.firm = "FIRM";
    offer(framed(soup::packet_type::unsequenced_data,
                 encode_into(
                     [&](dfr::mutable_packet_view out) {
                       return ouch::encode_enter_order(out, order);
                     },
                     ouch::kMaxMessageBytes)));
  }

  if (fill_shares > 0) {
    clock.advance(std::chrono::milliseconds{5});
    ouch::price at{};
    (void)ouch::price::from_dollars_and_ten_thousandths(150, 0).get(at);
    char detail[96];
    std::snprintf(detail, sizeof detail, "ORDER0001 filled %d of %u", fill_shares,
                  dfr_tools::shares_for(0));
    note("venue", '*', "a fill lands", detail, 0);
    venue::order_outcome outcome{};
    (void)host.execute(token_of("ORDER0001"), static_cast<std::uint32_t>(fill_shares), at,
                       clock.now(), emit)
        .get(outcome);
  }

  if (do_cancel && order_count >= 2) {
    note("client", 'U', "Cancel Order", "token=ORDER0002 all remaining", 0);
    const ouch::cancel_order request{.token = token_of("ORDER0002"), .intended_order_size = 0};
    offer(framed(soup::packet_type::unsequenced_data,
                 encode_into(
                     [&](dfr::mutable_packet_view out) {
                       return ouch::encode_cancel_order(out, request);
                     },
                     ouch::kMaxMessageBytes)));
  }

  note("client", 'O', "Logout Request", "", 0);
  offer(framed(soup::packet_type::logout_request, {}));

  return rendered([&](std::FILE* out) {
    dfr_tools::write_session_header(out, session_options, order_count, fill_shares, do_cancel);
    for (std::size_t i = 0; i < journal.size(); ++i) {
      const auto& value = journal[i];
      dfr_tools::write_wire_step(out, i,
                                 dfr_tools::wire_step{.from = value.from,
                                                      .type = value.type,
                                                      .name = value.name,
                                                      .detail = value.detail,
                                                      .sequence = value.sequence,
                                                      .server_next = value.server_next,
                                                      .client_next = value.client_next,
                                                      .live_orders = value.live_orders,
                                                      .shares_open = value.shares_open,
                                                      .phase = value.phase});
    }
    dfr_tools::write_session_summary(out, host.stats(), venue::name_of(host.ending()),
                                     host.orders().live_orders(), host.orders().accounts(),
                                     host.next_sequence(), cursor.next_sequence());
  });
}

// One Glimpse snapshot, as JSONL. The same output as `tools/glimpse --trace`.
//
// A separate export rather than a mode of the trace run, because a snapshot session is not a feed: it has no faults,
// no seed and no clock, and folding it into a function that takes six of those would be an interface shaped by
// convenience rather than by what it does.
EMSCRIPTEN_KEEPALIVE
const char* dfr_run_glimpse(int levels, double resume) {
  const auto depth = static_cast<std::size_t>(clamped(levels, 1, 16));
  const auto from = static_cast<std::uint64_t>(resume < 1 ? 1 : resume);
  const auto run = dfr_tools::run_glimpse_session(depth, from);

  return rendered([&](std::FILE* out) {
    dfr_tools::write_glimpse_header(
        out, dfr_tools::kTracedSymbol, dfr_tools::kGlimpseSession,
        static_cast<std::uint16_t>(run.venue.bids().size()),
        static_cast<std::uint16_t>(run.venue.asks().size()), run.venue.bids().best().at.raw(),
        run.venue.bids().best().size, run.venue.asks().best().at.raw(),
        run.venue.asks().best().size, run.resume_from);
    for (const auto& step : run.steps) {
      dfr_tools::write_glimpse_step(out, step);
    }
  });
}

}  // extern "C"

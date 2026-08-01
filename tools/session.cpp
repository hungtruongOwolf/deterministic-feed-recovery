// session: run an order-entry session, print what crossed the wire, and optionally record it.
//
// venue::order_session is the seam between a SoupBinTCP stream and an OUCH host, and until this tool
// existed the only way to look at it was to read a unit test. A seam benefits from being watched rather
// than only asserted: the interesting part is not that a field decodes, it is that the sequence number a
// server assigned is the one a client counting independently arrives at.
//
// So this drives a scripted session with a real stream_cursor running alongside the server, and prints
// both directions. `--trace FILE` writes the same conversation as JSONL for the viewer, which is why the
// journal is built first and rendered second: one recording, two outputs, so the page and the terminal
// cannot disagree.
//
// Every line is decoded through wire::soupbintcp and wire::ouch before it is shown. A tool that formatted
// bytes at offsets it knew would agree with itself about a layout that was wrong.
//
// Usage:  session [--orders N] [--fill N] [--no-cancel] [--trace FILE] [--quiet]

#include "support/session_render.hpp"
#include "support/session_trace.hpp"

#include <dfr/venue/order_session.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace ouch = dfr::wire::ouch;
namespace soup = dfr::wire::soupbintcp;
namespace venue = dfr::venue;

namespace {

using clock_type = dfr::manual_clock;
using session_type = venue::order_session<clock_type>;

struct options {
  int orders = 3;
  int fill = 40;
  bool cancel = true;
  bool quiet = false;
  const char* trace_path = nullptr;
};

// One arrow, with its strings owned: the views in dfr_tools::wire_step point into these.
struct recorded {
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

std::string encode_into(auto&& encode, std::size_t capacity) {
  std::vector<std::byte> scratch(capacity);
  std::size_t size = 0;
  if (encode(dfr::mutable_packet_view{scratch.data(), scratch.size()}).get(size) !=
      dfr::error::ok) {
    return {};
  }
  return std::string{reinterpret_cast<const char*>(scratch.data()), size};
}

std::string framed(soup::packet_type type, std::string_view payload) {
  return encode_into(
      [&](dfr::mutable_packet_view out) {
        return soup::encode_packet(out, type, dfr::packet_view{payload.data(), payload.size()});
      },
      soup::kMaxPacketBytes);
}

ouch::order_token token_of(std::string_view text) {
  ouch::order_token out;
  (void)ouch::order_token::from_text(text).get(out);
  return out;
}

std::string an_order(std::string_view token_text, int index) {
  ouch::enter_order order;
  order.token = token_of(token_text);
  order.order_side = index % 3 == 2 ? ouch::side::sell : ouch::side::buy;
  order.shares = dfr_tools::shares_for(index);
  order.stock = dfr_tools::symbol_for(index);
  (void)ouch::price::from_dollars_and_ten_thousandths(dfr_tools::price_for(index), 0)
      .get(order.limit);
  order.time_in_force = ouch::kSystemHours;
  order.firm = "FIRM";
  return encode_into(
      [&](dfr::mutable_packet_view out) { return ouch::encode_enter_order(out, order); },
      ouch::kMaxMessageBytes);
}

int usage() {
  std::fprintf(stderr,
               "usage: session [--orders N] [--fill N] [--no-cancel] [--trace FILE] [--quiet]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--quiet") {
      opts.quiet = true;
    } else if (arg == "--no-cancel") {
      opts.cancel = false;
    } else if (arg == "--orders" && i + 1 < argc) {
      opts.orders = std::atoi(argv[++i]);
    } else if (arg == "--fill" && i + 1 < argc) {
      opts.fill = std::atoi(argv[++i]);
    } else if (arg == "--trace" && i + 1 < argc) {
      opts.trace_path = argv[++i];
    } else {
      std::fprintf(stderr, "session: unrecognised argument %.*s\n", static_cast<int>(arg.size()),
                   arg.data());
      return usage();
    }
  }
  if (opts.orders < 1 || opts.orders > 64) {
    std::fprintf(stderr, "session: --orders must be between 1 and 64\n");
    return usage();
  }

  clock_type clock;
  const venue::order_session_options session_options{};
  session_type host{session_options, venue::order_entry_options{}};

  // A real client's cursor: it derives the sequence of every Sequenced Data Packet by counting, because
  // SoupBinTCP puts it nowhere in the packet. Agreement with the server is the only evidence either of
  // them is right.
  soup::stream_cursor cursor;
  std::vector<recorded> journal;
  std::vector<std::string> tokens;

  // What is still exposed, summed over the tokens this tool entered. The host does not offer iteration,
  // and inventing one for a report would be widening a library interface for a printout.
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
    journal.push_back(recorded{.from = std::string{from},
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

  // Everything the server emits goes through the client's cursor first, so the numbers recorded are the
  // ones a client would have derived rather than the ones the server happens to hold.
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
        // The client's half of the agreement: from here it counts for itself.
        cursor.accept_login(accepted.next_sequence);
        char detail[96];
        std::snprintf(detail, sizeof detail, "session=%.*s next sequence=%llu",
                      static_cast<int>(accepted.session.size()), accepted.session.data(),
                      static_cast<unsigned long long>(accepted.next_sequence));
        note("server", 'A', "Login Accepted", detail, 0);
        return;
      }
      case soup::packet_type::login_rejected: {
        soup::reject_reason reason{};
        if (soup::decode_login_rejected(next.frame.payload).get(reason) != dfr::error::ok) {
          return;
        }
        char detail[48];
        std::snprintf(detail, sizeof detail, "reason=%c", static_cast<char>(reason));
        note("server", 'J', "Login Rejected", detail, 0);
        return;
      }
      case soup::packet_type::sequenced_data: {
        auto what = dfr_tools::describe_ouch(next.frame.payload);
        note("server", 'S', std::move(what.name), std::move(what.detail), next.sequence);
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
    const auto err =
        host.offer(dfr::packet_view{bytes.data(), bytes.size()}, clock.now(), emit).get(taken);
    if (err != dfr::error::ok) {
      std::fprintf(stderr, "session: %s\n", dfr::to_string(err).data());
      std::exit(1);
    }
  };

  // ---- the script ---------------------------------------------------------

  note("client", 'L', "Login Request", "user=DFRUSR", 0);
  offer(encode_into(
      [](dfr::mutable_packet_view out) {
        return soup::encode_login_request(out, "DFRUSR", "DFRPASS", "", 0);
      },
      soup::kMaxPacketBytes));

  for (int i = 0; i < opts.orders; ++i) {
    char text[16];
    std::snprintf(text, sizeof text, "ORDER%04d", i + 1);
    tokens.emplace_back(text);
    char detail[96];
    std::snprintf(detail, sizeof detail, "token=%s %u %.*s %s @%u.00", text,
                  dfr_tools::shares_for(i),
                  static_cast<int>(dfr_tools::symbol_for(i).size()), dfr_tools::symbol_for(i).data(),
                  i % 3 == 2 ? "sell" : "buy", dfr_tools::price_for(i));
    note("client", 'U', "Enter Order", detail, 0);
    offer(framed(soup::packet_type::unsequenced_data, an_order(text, i)));
  }

  if (opts.fill > 0) {
    clock.advance(std::chrono::milliseconds{5});
    ouch::price at{};
    (void)ouch::price::from_dollars_and_ten_thousandths(150, 0).get(at);
    char detail[96];
    std::snprintf(detail, sizeof detail, "ORDER0001 filled %d of %u", opts.fill,
                  dfr_tools::shares_for(0));
    // Not an arrow from either side: the exchange's own matching, which this project deliberately does
    // not implement, so the caller drives it.
    note("venue", '*', "a fill lands", detail, 0);
    venue::order_outcome outcome{};
    const auto err = host.execute(token_of("ORDER0001"), static_cast<std::uint32_t>(opts.fill), at,
                                  clock.now(), emit)
                         .get(outcome);
    if (err != dfr::error::ok) {
      std::fprintf(stderr, "session: %s\n", dfr::to_string(err).data());
      return 1;
    }
  }

  if (opts.cancel && opts.orders >= 2) {
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

  // ---- the two outputs ---------------------------------------------------

  const bool agreed = cursor.next_sequence() == host.next_sequence();
  const bool balanced = host.orders().accounts();

  if (opts.trace_path != nullptr) {
    std::FILE* out = std::fopen(opts.trace_path, "w");
    if (out == nullptr) {
      std::fprintf(stderr, "session: cannot write %s\n", opts.trace_path);
      return 1;
    }
    dfr_tools::write_session_header(out, session_options, opts.orders, opts.fill, opts.cancel);
    for (std::size_t i = 0; i < journal.size(); ++i) {
      const auto& step = journal[i];
      dfr_tools::write_wire_step(out, i,
                                 dfr_tools::wire_step{.from = step.from,
                                                      .type = step.type,
                                                      .name = step.name,
                                                      .detail = step.detail,
                                                      .sequence = step.sequence,
                                                      .server_next = step.server_next,
                                                      .client_next = step.client_next,
                                                      .live_orders = step.live_orders,
                                                      .shares_open = step.shares_open,
                                                      .phase = step.phase});
    }
    dfr_tools::write_session_summary(out, host.stats(), venue::name_of(host.ending()),
                                     host.orders().live_orders(), balanced, host.next_sequence(),
                                     cursor.next_sequence());
    std::fclose(out);
    if (!opts.quiet) {
      std::printf("wrote %s: %zu steps\n", opts.trace_path, journal.size());
    }
  }

  if (!opts.quiet) {
    std::printf("An OUCH order-entry session over SoupBinTCP\n");
    std::printf("The client counts the server's stream itself; the sequence is never on the wire.\n\n");
    for (const auto& step : journal) {
      const char* arrow = step.from == "client" ? "client → server" : "server → client";
      if (step.from == "venue") {
        arrow = "   (exchange)  ";
      }
      // 24, not 12: a uint64 sequence is up to twenty digits, and "#%-4llu" of twenty digits is
      // twenty-one bytes plus a terminator. GCC did the arithmetic; the previous size silently
      // truncated a sequence above 99,999,999,999.
      char number[24] = "     ";
      if (step.sequence != 0) {
        std::snprintf(number, sizeof number, "#%-4llu",
                      static_cast<unsigned long long>(step.sequence));
      }
      std::printf("  %s  %s  %-16s %s\n", arrow, number, step.name.c_str(), step.detail.c_str());
    }

    const auto& stats = host.stats();
    std::printf("\n  session ended         %s\n", venue::name_of(host.ending()).data());
    std::printf("  orders in             %llu\n",
                static_cast<unsigned long long>(stats.orders_in));
    std::printf("  acknowledgements out  %llu\n",
                static_cast<unsigned long long>(stats.acknowledgements_out));
    std::printf("  live orders           %zu\n", host.orders().live_orders());
    std::printf("  accounting balances   %s\n", balanced ? "yes" : "NO");
    std::printf("\n  server's next sequence   %llu\n",
                static_cast<unsigned long long>(host.next_sequence()));
    std::printf("  client counted up to     %llu\n",
                static_cast<unsigned long long>(cursor.next_sequence()));
    std::printf("  %s\n", agreed ? "they agree, having never exchanged the number"
                                 : "THEY DISAGREE, the session and the cursor read the stream "
                                   "differently");
  }

  return agreed && balanced ? 0 : 1;
}

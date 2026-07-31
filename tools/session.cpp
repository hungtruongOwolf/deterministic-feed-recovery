// session — run an order-entry session and print what crossed the wire.
//
// venue::order_session is the seam between a SoupBinTCP stream and an OUCH host, and until this tool
// existed the only way to look at it was to read a unit test. A seam is exactly the thing that benefits
// from being watched rather than asserted: the interesting part is not that a field decodes, it is that
// the sequence number a server assigned is the one a client counting independently arrives at.
//
// So this drives a scripted session and prints both directions, byte counts and all, with the client's
// own cursor running alongside the server. The last line is the check that matters — the two numbers
// agree, and neither of them was ever on the wire.
//
// Every line printed is decoded, never read as an offset this file knows. A tool that formatted bytes by
// reaching into them would agree with itself about a layout that was wrong.
//
// Usage:  session [--orders N] [--fill N] [--cancel] [--quiet]

#include <dfr/venue/order_session.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
};

// The client half: it holds a cursor, so every server packet gets a number derived by counting rather
// than by being told. That is the whole point of printing this.
struct client {
  soup::stream_cursor cursor{};
  std::uint64_t sequenced_seen = 0;
  bool quiet = false;

  void on_frame(dfr::packet_view frame) {
    soup::sequenced_packet next;
    if (cursor.next(frame).get(next) != dfr::error::ok) {
      std::printf("  server → client   [undecodable, %zu bytes]\n", frame.size());
      return;
    }
    describe(next);
  }

  void describe(const soup::sequenced_packet& next) {
    switch (next.frame.type) {
      case soup::packet_type::login_accepted: {
        soup::login_accepted accepted;
        if (soup::decode_login_accepted(next.frame.payload).get(accepted) != dfr::error::ok) {
          return;
        }
        // Adopting the position is the client's half of the sequence agreement: from here it counts.
        cursor.accept_login(accepted.next_sequence);
        std::printf("  server → client   Login Accepted    session=%.*s  next sequence=%llu\n",
                    static_cast<int>(accepted.session.size()), accepted.session.data(),
                    static_cast<unsigned long long>(accepted.next_sequence));
        return;
      }
      case soup::packet_type::login_rejected: {
        soup::reject_reason reason{};
        if (soup::decode_login_rejected(next.frame.payload).get(reason) != dfr::error::ok) {
          return;
        }
        std::printf("  server → client   Login Rejected    reason=%c\n",
                    static_cast<char>(reason));
        return;
      }
      case soup::packet_type::sequenced_data:
        ++sequenced_seen;
        std::printf("  server → client   #%-4llu  %s\n",
                    static_cast<unsigned long long>(next.sequence),
                    describe_ouch(next.frame.payload).c_str());
        return;
      case soup::packet_type::server_heartbeat:
        std::printf("  server → client   heartbeat\n");
        return;
      case soup::packet_type::end_of_session:
        std::printf("  server → client   End Of Session\n");
        return;
      default:
        std::printf("  server → client   type '%c'\n", static_cast<char>(next.frame.type));
        return;
    }
  }

  // Decoded through wire::ouch, so a wrong layout here would be a wrong layout there.
  static std::string describe_ouch(dfr::packet_view message) {
    if (message.empty()) {
      return "[empty]";
    }
    char line[160];
    switch (static_cast<ouch::outbound_type>(message.u8_at(0))) {
      case ouch::outbound_type::accepted: {
        ouch::accepted decoded;
        if (ouch::decode_accepted(message).get(decoded) != dfr::error::ok) {
          break;
        }
        const auto text = decoded.token.text();
        std::snprintf(line, sizeof line, "Accepted      token=%-9.*s  shares=%u  state=%c",
                      static_cast<int>(text.size()), text.data(), decoded.shares_accepted,
                      static_cast<char>(decoded.order.state));
        return line;
      }
      case ouch::outbound_type::executed: {
        ouch::executed decoded;
        if (ouch::decode_executed(message).get(decoded) != dfr::error::ok) {
          break;
        }
        const auto text = decoded.token.text();
        std::snprintf(line, sizeof line, "Executed      token=%-9.*s  shares=%u  match=%llu",
                      static_cast<int>(text.size()), text.data(), decoded.shares_this_fill,
                      static_cast<unsigned long long>(decoded.match_number));
        return line;
      }
      case ouch::outbound_type::canceled: {
        ouch::canceled decoded;
        if (ouch::decode_canceled(message).get(decoded) != dfr::error::ok) {
          break;
        }
        const auto text = decoded.token.text();
        const auto why = ouch::name_of_cancel_reason(decoded.reason);
        std::snprintf(line, sizeof line, "Canceled      token=%-9.*s  removed=%u  reason=%.*s",
                      static_cast<int>(text.size()), text.data(), decoded.shares_decremented,
                      static_cast<int>(why.size()), why.data());
        return line;
      }
      default:
        break;
    }
    std::snprintf(line, sizeof line, "type '%c'  (%zu bytes)", static_cast<char>(message.u8_at(0)),
                  message.size());
    return line;
  }
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
        return soup::encode_packet(out, type,
                                   dfr::packet_view{payload.data(), payload.size()});
      },
      soup::kMaxPacketBytes);
}

ouch::order_token token_of(std::string_view text) {
  ouch::order_token out;
  (void)ouch::order_token::from_text(text).get(out);
  return out;
}

std::string an_order(std::string_view token_text, std::uint32_t shares) {
  ouch::enter_order order;
  order.token = token_of(token_text);
  order.order_side = ouch::side::buy;
  order.shares = shares;
  order.stock = "AAPL";
  (void)ouch::price::from_dollars_and_ten_thousandths(150, 0).get(order.limit);
  order.time_in_force = ouch::kSystemHours;
  order.firm = "FIRM";
  return encode_into(
      [&](dfr::mutable_packet_view out) { return ouch::encode_enter_order(out, order); },
      ouch::kMaxMessageBytes);
}

int usage() {
  std::fprintf(stderr, "usage: session [--orders N] [--fill N] [--cancel] [--quiet]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--quiet") {
      opts.quiet = true;
    } else if (arg == "--cancel") {
      opts.cancel = true;
    } else if (arg == "--orders" && i + 1 < argc) {
      opts.orders = std::atoi(argv[++i]);
    } else if (arg == "--fill" && i + 1 < argc) {
      opts.fill = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr, "session: unrecognised argument %.*s\n",
                   static_cast<int>(arg.size()), arg.data());
      return usage();
    }
  }
  if (opts.orders < 1 || opts.orders > 64) {
    std::fprintf(stderr, "session: --orders must be between 1 and 64\n");
    return usage();
  }

  clock_type clock;
  session_type host{venue::order_session_options{}, venue::order_entry_options{}};
  client peer;
  peer.quiet = opts.quiet;

  const auto emit = [&](dfr::packet_view frame) {
    if (!opts.quiet) {
      peer.on_frame(frame);
    } else {
      soup::sequenced_packet next;
      if (peer.cursor.next(frame).get(next) == dfr::error::ok) {
        if (next.frame.type == soup::packet_type::login_accepted) {
          soup::login_accepted accepted;
          if (soup::decode_login_accepted(next.frame.payload).get(accepted) == dfr::error::ok) {
            peer.cursor.accept_login(accepted.next_sequence);
          }
        } else if (next.frame.type == soup::packet_type::sequenced_data) {
          ++peer.sequenced_seen;
        }
      }
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
    return taken;
  };

  std::printf("An OUCH order-entry session over SoupBinTCP\n");
  std::printf("The client counts the server's stream itself; the sequence is never on the wire.\n\n");

  if (!opts.quiet) {
    std::printf("  client → server   Login Request     user=DFRUSR\n");
  }
  offer(encode_into(
      [](dfr::mutable_packet_view out) {
        return soup::encode_login_request(out, "DFRUSR", "DFRPASS", "", 0);
      },
      soup::kMaxPacketBytes));

  for (int i = 0; i < opts.orders; ++i) {
    char text[16];
    std::snprintf(text, sizeof text, "ORDER%04d", i + 1);
    if (!opts.quiet) {
      std::printf("  client → server   Enter Order       token=%s  shares=200\n", text);
    }
    offer(framed(soup::packet_type::unsequenced_data, an_order(text, 200)));
  }

  if (opts.fill > 0) {
    clock.advance(std::chrono::milliseconds{5});
    ouch::price at{};
    (void)ouch::price::from_dollars_and_ten_thousandths(150, 0).get(at);
    if (!opts.quiet) {
      std::printf("  (exchange)        a fill lands on ORDER0001: %d shares\n", opts.fill);
    }
    venue::order_outcome outcome{};
    const auto err =
        host.execute(token_of("ORDER0001"), static_cast<std::uint32_t>(opts.fill), at,
                     clock.now(), emit)
            .get(outcome);
    if (err != dfr::error::ok) {
      std::fprintf(stderr, "session: %s\n", dfr::to_string(err).data());
      return 1;
    }
  }

  if (opts.cancel && opts.orders >= 2) {
    if (!opts.quiet) {
      std::printf("  client → server   Cancel Order      token=ORDER0002  (all remaining)\n");
    }
    const ouch::cancel_order request{.token = token_of("ORDER0002"), .intended_order_size = 0};
    offer(framed(soup::packet_type::unsequenced_data,
                 encode_into(
                     [&](dfr::mutable_packet_view out) {
                       return ouch::encode_cancel_order(out, request);
                     },
                     ouch::kMaxMessageBytes)));
  }

  if (!opts.quiet) {
    std::printf("  client → server   Logout Request\n");
  }
  offer(framed(soup::packet_type::logout_request, {}));

  const auto& stats = host.stats();
  std::printf("\n");
  std::printf("  session ended         %s\n", venue::name_of(host.ending()).data());
  std::printf("  orders in             %llu\n",
              static_cast<unsigned long long>(stats.orders_in));
  std::printf("  acknowledgements out  %llu\n",
              static_cast<unsigned long long>(stats.acknowledgements_out));
  std::printf("  live orders           %zu\n", host.orders().live_orders());
  std::printf("  accounting balances   %s\n", host.orders().accounts() ? "yes" : "NO");

  // The line this tool exists for. One side assigned these numbers and the other side counted them, and
  // SoupBinTCP puts the sequence of a Sequenced Data Packet nowhere in the packet — so agreement is the
  // only evidence either of them is right.
  const bool agreed = peer.cursor.next_sequence() == host.next_sequence();
  std::printf("\n  server's next sequence   %llu\n",
              static_cast<unsigned long long>(host.next_sequence()));
  std::printf("  client counted up to     %llu\n",
              static_cast<unsigned long long>(peer.cursor.next_sequence()));
  std::printf("  %s\n", agreed ? "they agree, having never exchanged the number"
                               : "THEY DISAGREE — the session and the cursor read the stream differently");
  return agreed && host.orders().accounts() ? 0 : 1;
}

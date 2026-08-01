// corpus_gen: drives several real order-entry sessions and writes every raw byte crossing the wire, both
// directions, as fuzz seed material.
//
// iextp/deep/moldudp64 all have a real IEX HIST capture to draw seeds from (scripts/extract-corpus.py).
// ouch and soupbintcp do not: they are a private client-exchange session, not multicast market data, and no
// free public capture of one exists. Rather than hand-writing bytes in Python and risking a second,
// independent (and possibly wrong) implementation of the wire layout, this calls the library's own encoders
// through a real venue::order_session, so every seed byte-for-byte matches what the real server or a real
// client actually produces.
//
// Several short scripts rather than one long one, so the corpus has variety in message *type* (login
// accepted and rejected, enter/replace/cancel/modify, an execution, a heartbeat, both ends of a session)
// rather than only variety in field values within one flow.

#include <dfr/venue/order_session.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ouch = dfr::wire::ouch;
namespace soup = dfr::wire::soupbintcp;
namespace venue = dfr::venue;
namespace fs = std::filesystem;

namespace {

using clock_type = dfr::manual_clock;
using session_type = venue::order_session<clock_type>;

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

std::string enter(std::string_view text, ouch::side side, std::uint32_t shares,
                  std::string_view stock, std::uint32_t dollars) {
  ouch::enter_order order;
  order.token = token_of(text);
  order.order_side = side;
  order.shares = shares;
  order.stock = stock;
  (void)ouch::price::from_dollars_and_ten_thousandths(dollars, 0).get(order.limit);
  order.time_in_force = ouch::kSystemHours;
  order.firm = "FIRM";
  return framed(soup::packet_type::unsequenced_data,
               encode_into([&](dfr::mutable_packet_view out) { return ouch::encode_enter_order(out, order); },
                          ouch::kMaxMessageBytes));
}

// Writes one file per call, under `dir`, named by an incrementing counter so nothing collides.
class corpus_writer {
 public:
  explicit corpus_writer(fs::path dir) : dir_(std::move(dir)) {
    fs::create_directories(dir_);
  }

  void write(std::string_view bytes) {
    if (bytes.empty()) {
      return;
    }
    const auto path = dir_ / ("seed-" + std::to_string(next_++));
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) {
      return;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
  }

  [[nodiscard]] std::size_t count() const noexcept { return next_; }

 private:
  fs::path dir_;
  std::size_t next_{0};
};

// One session, scripted by the caller. Every raw SoupBinTCP frame goes to `soup_out`; the OUCH payload
// inside an unsequenced or sequenced data frame also goes to `ouch_out`, since those are the bytes an OUCH
// decoder actually sees.
struct wire_session {
  session_type host;
  clock_type clock;
  corpus_writer& soup_out;
  corpus_writer& ouch_out;

  wire_session(venue::order_session_options options, corpus_writer& s, corpus_writer& o)
      : host(options, venue::order_entry_options{}), soup_out(s), ouch_out(o) {}

  void send(std::string_view frame) {
    soup_out.write(frame);
    if (frame.size() >= soup::kFrameOverhead) {
      const auto type = static_cast<soup::packet_type>(frame[soup::kTypeOffset]);
      if (type == soup::packet_type::unsequenced_data) {
        ouch_out.write(frame.substr(soup::kFrameOverhead));
      }
    }
    std::size_t taken = 0;
    const auto emit = [&](dfr::packet_view out_frame) {
      const std::string out{reinterpret_cast<const char*>(out_frame.data()), out_frame.size()};
      soup_out.write(out);
      if (out.size() >= soup::kFrameOverhead &&
          static_cast<soup::packet_type>(out[soup::kTypeOffset]) == soup::packet_type::sequenced_data) {
        ouch_out.write(out.substr(soup::kFrameOverhead));
      }
    };
    (void)host.offer(dfr::packet_view{frame.data(), frame.size()}, clock.now(), emit).get(taken);
  }

  void poll_forward(std::chrono::milliseconds by) {
    clock.advance(by);
    const auto emit = [&](dfr::packet_view out_frame) {
      soup_out.write(std::string{reinterpret_cast<const char*>(out_frame.data()), out_frame.size()});
    };
    host.poll(clock.now(), emit);
  }
};

}  // namespace

int main(int argc, char** argv) {
  fs::path root = argc > 1 ? argv[1] : "fuzz/corpus";
  corpus_writer soup_out(root / "soupbintcp");
  corpus_writer ouch_out(root / "ouch");

  // 1. The baseline flow: login, three orders on different sides and symbols, a cancel, a logout.
  {
    wire_session s{{}, soup_out, ouch_out};
    s.send(encode_into([](dfr::mutable_packet_view out) {
                    return soup::encode_login_request(out, "DFRUSR", "DFRPASS", "", 0);
                  }, soup::kMaxPacketBytes));
    s.send(enter("ORDER0001", ouch::side::buy, 100, "IEXT", 20));
    s.send(enter("ORDER0002", ouch::side::sell, 200, "WWE", 15));
    s.send(enter("ORDER0003", ouch::side::sell_short, 50, "VIAV", 8));
    const ouch::cancel_order cancel{.token = token_of("ORDER0002"), .intended_order_size = 0};
    s.send(framed(soup::packet_type::unsequenced_data,
                  encode_into([&](dfr::mutable_packet_view out) { return ouch::encode_cancel_order(out, cancel); },
                             ouch::kMaxMessageBytes)));
    s.send(framed(soup::packet_type::logout_request, {}));
  }

  // 2. Wrong credentials: the one way to reach login_rejected / Login Rejected on the wire.
  {
    wire_session s{{}, soup_out, ouch_out};
    s.send(encode_into([](dfr::mutable_packet_view out) {
                    return soup::encode_login_request(out, "DFRUSR", "wrong", "", 0);
                  }, soup::kMaxPacketBytes));
  }

  // 3. Replace, then modify, then an execution: the acks a fuzzer would otherwise never see a real example
  // of (Order Replaced, Order Modified, Order Executed each carry fields the enter/cancel acks do not).
  {
    wire_session s{{}, soup_out, ouch_out};
    s.send(encode_into([](dfr::mutable_packet_view out) {
                    return soup::encode_login_request(out, "DFRUSR", "DFRPASS", "", 0);
                  }, soup::kMaxPacketBytes));
    s.send(enter("REPL0001", ouch::side::buy, 500, "IEXT", 20));

    ouch::replace_order replace;
    replace.existing_token = token_of("REPL0001");
    replace.replacement_token = token_of("REPL0002");
    replace.total_shares_liable = 400;
    replace.limit = {};
    (void)ouch::price::from_dollars_and_ten_thousandths(21, 0).get(replace.limit);
    replace.time_in_force = ouch::kSystemHours;
    s.send(framed(soup::packet_type::unsequenced_data,
                  encode_into([&](dfr::mutable_packet_view out) { return ouch::encode_replace_order(out, replace); },
                             ouch::kMaxMessageBytes)));

    const ouch::modify_order modify{
        .token = token_of("REPL0002"), .order_side = ouch::side::buy, .total_shares_liable = 300};
    s.send(framed(soup::packet_type::unsequenced_data,
                  encode_into([&](dfr::mutable_packet_view out) { return ouch::encode_modify_order(out, modify); },
                             ouch::kMaxMessageBytes)));

    ouch::price fill_at{};
    (void)ouch::price::from_dollars_and_ten_thousandths(21, 0).get(fill_at);
    venue::order_outcome outcome{};
    const auto emit = [&](dfr::packet_view frame) {
      soup_out.write(std::string{reinterpret_cast<const char*>(frame.data()), frame.size()});
    };
    (void)s.host.execute(token_of("REPL0002"), 150, fill_at, s.clock.now(), emit).get(outcome);
  }

  // 4. A quiet session: only heartbeats and an End Of Session, the two frame types nothing above produces.
  {
    wire_session s{{}, soup_out, ouch_out};
    s.send(encode_into([](dfr::mutable_packet_view out) {
                    return soup::encode_login_request(out, "DFRUSR", "DFRPASS", "", 0);
                  }, soup::kMaxPacketBytes));
    s.poll_forward(std::chrono::milliseconds{1'200});
    s.poll_forward(std::chrono::milliseconds{1'200});
    const auto emit = [&](dfr::packet_view frame) {
      soup_out.write(std::string{reinterpret_cast<const char*>(frame.data()), frame.size()});
    };
    s.host.close(s.clock.now(), emit);
  }

  std::printf("corpus_gen: %zu soupbintcp seeds, %zu ouch seeds, under %s\n", soup_out.count(),
             ouch_out.count(), root.string().c_str());
  return 0;
}

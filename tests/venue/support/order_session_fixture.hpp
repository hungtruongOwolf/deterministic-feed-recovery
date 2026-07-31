// Shared setup for the order-entry session tests: a place to put bytes in, and a place to read them out.
//
// Separate from order_entry_fixture.hpp because these are session concerns — framing a client's stream
// and decoding the server's — and a test about token rules needs none of it, per docs/STYLE.md §1.10.
//
// Nothing here inspects a byte it wrote itself. Frames go out through the encoder and come back through
// the decoder, so a session that is wrong in the same way as this file would still fail.

#ifndef DFR_TESTS_VENUE_SUPPORT_ORDER_SESSION_FIXTURE_HPP
#define DFR_TESTS_VENUE_SUPPORT_ORDER_SESSION_FIXTURE_HPP

#include <dfr/venue/order_session.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dfr_test::order_session {

namespace ouch = dfr::wire::ouch;
namespace soup = dfr::wire::soupbintcp;
namespace venue = dfr::venue;

using test_session = venue::order_session<dfr::manual_clock>;
using test_time = dfr::manual_clock::time_point;

namespace {

inline test_time at_ms(std::int64_t millis) {
  return test_time{} + std::chrono::milliseconds{millis};
}

// Collects the frames the session emitted, copied because the session reuses one scratch buffer.
struct wire_log {
  std::vector<std::string> frames;

  void operator()(dfr::packet_view frame) {
    frames.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
  }

  [[nodiscard]] std::size_t count() const { return frames.size(); }
  void clear() { frames.clear(); }

  [[nodiscard]] dfr::packet_view bytes_at(std::size_t i) const {
    REQUIRE(i < frames.size());
    return dfr::packet_view{frames[i].data(), frames[i].size()};
  }

  // Decoded, never read as raw offsets: the decoder is what checks the encoder.
  [[nodiscard]] soup::packet at(std::size_t i) const {
    soup::packet out;
    REQUIRE(soup::decode(bytes_at(i)).get(out) == dfr::error::ok);
    return out;
  }

  [[nodiscard]] std::size_t count_of(soup::packet_type type) const {
    std::size_t found = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
      if (at(i).type == type) {
        ++found;
      }
    }
    return found;
  }

  // Every frame end to end, as a client's socket would have received them.
  [[nodiscard]] std::string joined() const {
    std::string out;
    for (const auto& frame : frames) {
      out += frame;
    }
    return out;
  }
};

// The bytes a client has sent but the session has not yet consumed.
struct client_stream {
  std::string bytes;

  template <typename Encode>
  void put(Encode&& encode) {
    std::array<std::byte, soup::kMaxPacketBytes> scratch{};
    std::size_t size = 0;
    REQUIRE(encode(dfr::mutable_packet_view{scratch.data(), scratch.size()}).get(size) ==
            dfr::error::ok);
    bytes.append(reinterpret_cast<const char*>(scratch.data()), size);
  }

  void put_order(dfr::packet_view message) {
    put([&](dfr::mutable_packet_view out) {
      return soup::encode_packet(out, soup::packet_type::unsequenced_data, message);
    });
  }

  void put_bare(soup::packet_type type) {
    put([&](dfr::mutable_packet_view out) { return soup::encode_bare(out, type); });
  }

  void put_login(std::string_view user = "DFRUSR", std::string_view pass = "DFRPASS",
                 std::string_view session = "") {
    put([&](dfr::mutable_packet_view out) {
      return soup::encode_login_request(out, user, pass, session, 0);
    });
  }

  [[nodiscard]] dfr::packet_view view() const {
    return dfr::packet_view{bytes.data(), bytes.size()};
  }
  [[nodiscard]] std::size_t size() const { return bytes.size(); }

  void consume(std::size_t count) { bytes.erase(0, count); }
  void clear() { bytes.clear(); }
};

inline ouch::order_token token(std::string_view text) {
  ouch::order_token out;
  REQUIRE(ouch::order_token::from_text(text).get(out) == dfr::error::ok);
  return out;
}

inline ouch::price at_dollars(std::uint32_t dollars) {
  ouch::price out;
  REQUIRE(ouch::price::from_dollars_and_ten_thousandths(dollars, 0).get(out) == dfr::error::ok);
  return out;
}

inline ouch::enter_order buy(std::string_view text, std::uint32_t shares) {
  ouch::enter_order order;
  order.token = token(text);
  order.order_side = ouch::side::buy;
  order.shares = shares;
  order.stock = "AAPL";
  order.limit = at_dollars(150);
  order.time_in_force = ouch::kSystemHours;
  order.firm = "FIRM";
  return order;
}

// An OUCH message as the bytes a client would put in an Unsequenced Data Packet.
template <typename Encode>
inline std::string encoded(Encode&& encode) {
  std::array<std::byte, ouch::kMaxMessageBytes> scratch{};
  std::size_t size = 0;
  REQUIRE(encode(dfr::mutable_packet_view{scratch.data(), scratch.size()}).get(size) ==
          dfr::error::ok);
  return std::string{reinterpret_cast<const char*>(scratch.data()), size};
}

inline void put_enter(client_stream& stream, std::string_view token_text, std::uint32_t shares) {
  const auto message = encoded([&](dfr::mutable_packet_view out) {
    return ouch::encode_enter_order(out, buy(token_text, shares));
  });
  stream.put_order(dfr::packet_view{message.data(), message.size()});
}

inline test_session fresh_session(bool heartbeats = false) {
  venue::order_session_options options;
  options.send_heartbeats = heartbeats;
  return test_session{options, venue::order_entry_options{}};
}

// Logs in and hands back the Login Accepted, because almost every test needs both.
inline soup::login_accepted log_in(test_session& host, client_stream& stream, wire_log& out,
                                   test_time now) {
  stream.put_login();
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), now, out).get(taken) == dfr::error::ok);
  stream.consume(taken);
  REQUIRE(host.phase() == venue::session_phase::established);
  REQUIRE(out.count() >= 1);

  soup::login_accepted accepted;
  REQUIRE(soup::decode_login_accepted(out.at(out.count() - 1).payload).get(accepted) ==
          dfr::error::ok);
  return accepted;
}

}  // namespace

}  // namespace dfr_test::order_session

#endif  // DFR_TESTS_VENUE_SUPPORT_ORDER_SESSION_FIXTURE_HPP

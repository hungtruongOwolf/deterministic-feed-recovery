// Recording an order-entry session as JSONL, for a viewer to draw.
//
// Same rule as dfr/trace/event.hpp, applied to a different subject: every line carries the *resulting*
// state, so a drawing can be made from one line without replaying the conversation. A viewer that
// reconstructed the sequence counters from the arrows would be a second implementation of the counting,
// written in TypeScript by somebody reading the C++, and the whole point of this session is that two
// independent counts agree, which a viewer doing its own third count could not demonstrate.
//
// So both counters are written on every line: what the server assigned, and what a real client cursor
// arrived at by counting. The drawing compares two numbers it was given rather than computing either.

#ifndef DFR_TOOLS_SUPPORT_SESSION_TRACE_HPP
#define DFR_TOOLS_SUPPORT_SESSION_TRACE_HPP

#include <dfr/venue/order_session.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace dfr_tools {

// The symbols and prices the scripted session uses.
//
// Eight orders all reading `AAPL @150.00` looked exactly like what it was: a hardcoded script. The protocol
// behaviour does not depend on the symbol, so varying it changes nothing that is being tested, and it changes
// a great deal about whether a reader believes the page is running anything.
//
// Derived from the order's index rather than randomised, because the session trace is a committed fixture and
// has to reproduce byte for byte.
inline constexpr std::string_view kSessionSymbols[] = {"AAPL", "MSFT", "NVDA", "TSLA",
                                                       "AMZN", "GOOG", "META", "AMD"};

[[nodiscard]] inline std::string_view symbol_for(int order_index) noexcept {
  const auto count = static_cast<int>(sizeof(kSessionSymbols) / sizeof(kSessionSymbols[0]));
  return kSessionSymbols[order_index % count];
}

// A price that differs per symbol, in whole dollars. Round numbers on purpose: the price arithmetic is tested
// exhaustively in the OUCH unit tests, and a session transcript is not the place to also be demonstrating
// sub-penny rounding.
[[nodiscard]] inline std::uint32_t price_for(int order_index) noexcept {
  static constexpr std::uint32_t dollars[] = {150, 410, 880, 240, 190, 175, 520, 165};
  const auto count = static_cast<int>(sizeof(dollars) / sizeof(dollars[0]));
  return dollars[order_index % count];
}

// Shares that are not all 200, for the same reason.
[[nodiscard]] inline std::uint32_t shares_for(int order_index) noexcept {
  static constexpr std::uint32_t shares[] = {200, 500, 100, 750, 300, 1'000, 150, 400};
  const auto count = static_cast<int>(sizeof(shares) / sizeof(shares[0]));
  return shares[order_index % count];
}

// One arrow on the ladder.
struct wire_step {
  // "client" or "server". Which way the arrow points, and there are only two parties.
  std::string_view from{};
  // The SoupBinTCP type byte, so a reader can check it against the specification.
  char type{' '};
  // What it is, in the specification's own words: "Login Accepted", "Enter Order".
  std::string_view name{};
  // The fields worth showing, already formatted. Formatted here because the decoding lives here.
  std::string_view detail{};
  // The sequence this packet carried, for a Sequenced Data Packet. Zero for the rest, because they have
  // none: not "unknown", none.
  std::uint64_t sequence{0};

  // ---- the resulting state, so the viewer needs no domain logic ----------
  std::uint64_t server_next{0};
  // What the client's own cursor has counted up to. The number that must match, and never travels.
  std::uint64_t client_next{0};
  std::size_t live_orders{0};
  std::uint64_t shares_open{0};
  std::string_view phase{};
};

inline void write_session_header(std::FILE* out, const dfr::venue::order_session_options& options,
                                 int orders, int fill, bool cancel) {
  std::fprintf(out,
               "{\"kind\":\"session\",\"schema\":\"dfr-session/1\",\"session\":\"%.*s\","
               "\"user\":\"%.*s\",\"orders\":%d,\"fill\":%d,\"cancel\":%s,"
               "\"first_sequence\":%llu,\"limits\":["
               "{\"claim\":\"the sequence the server assigned and the sequence a client counted\","
               "\"status\":\"measured\","
               "\"note\":\"an independent stream_cursor counted the server's own frames\"},"
               "{\"claim\":\"every share liable is executed, canceled or open\","
               "\"status\":\"measured\",\"note\":\"summed across the host after each step\"},"
               "{\"claim\":\"OUCH 4.2 field layouts\",\"status\":\"not-measurable\","
               "\"note\":\"no NASDAQ session to check against; layouts are from the specification "
               "and are verified only against this library's own decoder\"},"
               "{\"claim\":\"matching\",\"status\":\"not-measurable\","
               "\"note\":\"deliberately absent: executions are driven by the caller, so the "
               "protocol behaviour around matching is what gets tested\"}"
               "]}\n",
               static_cast<int>(options.session_id.size()), options.session_id.data(),
               static_cast<int>(options.username.size()), options.username.data(), orders, fill,
               cancel ? "true" : "false",
               static_cast<unsigned long long>(options.first_sequence));
}

inline void write_wire_step(std::FILE* out, std::size_t step, const wire_step& value) {
  std::fprintf(out,
               "{\"kind\":\"wire\",\"step\":%zu,\"from\":\"%.*s\",\"type\":\"%c\","
               "\"name\":\"%.*s\",\"detail\":\"%.*s\",\"sequence\":%llu,"
               "\"server_next\":%llu,\"client_next\":%llu,\"live_orders\":%zu,"
               "\"shares_open\":%llu,\"phase\":\"%.*s\"}\n",
               step, static_cast<int>(value.from.size()), value.from.data(), value.type,
               static_cast<int>(value.name.size()), value.name.data(),
               static_cast<int>(value.detail.size()), value.detail.data(),
               static_cast<unsigned long long>(value.sequence),
               static_cast<unsigned long long>(value.server_next),
               static_cast<unsigned long long>(value.client_next), value.live_orders,
               static_cast<unsigned long long>(value.shares_open),
               static_cast<int>(value.phase.size()), value.phase.data());
}

inline void write_session_summary(std::FILE* out, const dfr::venue::order_session_stats& stats,
                                  std::string_view ending, std::size_t live_orders, bool accounts,
                                  std::uint64_t server_next, std::uint64_t client_next) {
  std::fprintf(out,
               "{\"kind\":\"session_summary\",\"ending\":\"%.*s\",\"packets_in\":%llu,"
               "\"packets_out\":%llu,\"orders_in\":%llu,\"acknowledgements_out\":%llu,"
               "\"live_orders\":%zu,\"accounts\":%s,\"server_next\":%llu,\"client_next\":%llu,"
               "\"agreed\":%s}\n",
               static_cast<int>(ending.size()), ending.data(),
               static_cast<unsigned long long>(stats.packets_in),
               static_cast<unsigned long long>(stats.packets_out),
               static_cast<unsigned long long>(stats.orders_in),
               static_cast<unsigned long long>(stats.acknowledgements_out), live_orders,
               accounts ? "true" : "false", static_cast<unsigned long long>(server_next),
               static_cast<unsigned long long>(client_next),
               server_next == client_next ? "true" : "false");
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_SESSION_TRACE_HPP

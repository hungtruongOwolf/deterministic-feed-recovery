// Recording a Glimpse session as JSONL, for a viewer to draw.
//
// The third defence is the one a visitor has never seen work. In the film it is a plane the escalation marker falls
// onto — correct, and it shows the *consequence* of reaching a snapshot rather than the snapshot itself. What a
// snapshot actually is, and the only thing that makes it believable, is that a client with nothing but bytes ends up
// with the venue's book.
//
// So this records both books at every frame: the venue's, which does not change, and the client's, which fills up.
// The drawing is then a comparison a reader can watch converge, and the last frame carries the sequence the client
// resumes the live feed from — the one number a snapshot protocol exists to deliver.
//
// Same rule as every other trace here: the C++ decides, the viewer draws. The client's book is rebuilt *by a real
// consumer* reading the frames the service emitted, not by the service reporting what it sent. Those are different
// claims, and only the first one is worth making.

#ifndef DFR_TOOLS_SUPPORT_GLIMPSE_TRACE_HPP
#define DFR_TOOLS_SUPPORT_GLIMPSE_TRACE_HPP

#include <dfr/book/order_book.hpp>
#include <dfr/wire/deep.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace dfr_tools {

// One frame of the snapshot session, with both books as they stand after it.
struct glimpse_step {
  std::size_t step{0};
  /** The SoupBinTCP type byte, so a reader can check it against the specification. */
  char type{' '};
  /** "Login Accepted", "Begin Snapshot", "Price Level", "End Of Snapshot", "End Of Session". */
  std::string_view name{};
  // Owned, because a view into a temporary is how a trace ends up printing freed memory.
  std::string detail{};

  // The client's book after this frame — the one being rebuilt.
  std::int64_t bid{0};
  std::uint32_t bid_size{0};
  std::int64_t ask{0};
  std::uint32_t ask_size{0};
  std::uint16_t bid_levels{0};
  std::uint16_t ask_levels{0};

  /** Non-zero once End Of Snapshot has arrived: where the client resumes the live feed. */
  std::uint64_t resume_from{0};
  /** Whether the client's book now equals the venue's. The thing the drawing is for. */
  bool matches{false};
};

inline void write_glimpse_header(std::FILE* out, std::string_view symbol, std::uint32_t session,
                                 std::uint16_t venue_bid_levels, std::uint16_t venue_ask_levels,
                                 std::int64_t venue_bid, std::uint32_t venue_bid_size,
                                 std::int64_t venue_ask, std::uint32_t venue_ask_size,
                                 std::uint64_t resume_from) {
  std::fprintf(out,
               "{\"kind\":\"glimpse\",\"schema\":\"dfr-glimpse/1\",\"symbol\":\"%.*s\","
               "\"session\":%u,\"venue_bid\":%lld,\"venue_bid_size\":%u,\"venue_ask\":%lld,"
               "\"venue_ask_size\":%u,\"venue_bid_levels\":%u,\"venue_ask_levels\":%u,"
               "\"resume_from\":%llu,\"limits\":["
               "{\"claim\":\"the client rebuilds the venue's book from the frames alone\","
               "\"status\":\"measured\",\"note\":\"a real stream_cursor frames the bytes and a real "
               "order_book applies them; the service reports nothing about what it sent\"},"
               "{\"claim\":\"the resume sequence is the next message, not the last included\","
               "\"status\":\"measured\",\"note\":\"asserted in tests/integration/glimpse_test.cpp\"},"
               "{\"claim\":\"a snapshot carries state, not history\",\"status\":\"measured\","
               "\"note\":\"the levels match and the trade count does not, because a snapshot is "
               "O(depth) rather than O(messages) — which is why it can be served to a client that "
               "cannot catch up by replay\"},"
               "{\"claim\":\"Glimpse field layouts against a real venue\",\"status\":"
               "\"not-measurable\",\"note\":\"no NASDAQ or IEX Glimpse session to check against; the "
               "framing is SoupBinTCP, which is verified, and the two markers are this project's own\"}"
               "]}\n",
               static_cast<int>(symbol.size()), symbol.data(), session,
               static_cast<long long>(venue_bid), venue_bid_size,
               static_cast<long long>(venue_ask), venue_ask_size, venue_bid_levels,
               venue_ask_levels, static_cast<unsigned long long>(resume_from));
}

inline void write_glimpse_step(std::FILE* out, const glimpse_step& value) {
  std::fprintf(out,
               "{\"kind\":\"frame\",\"step\":%zu,\"type\":\"%c\",\"name\":\"%.*s\","
               "\"detail\":\"%.*s\",\"bid\":%lld,\"bid_size\":%u,\"ask\":%lld,\"ask_size\":%u,"
               "\"bid_levels\":%u,\"ask_levels\":%u,\"resume_from\":%llu,\"matches\":%s}\n",
               value.step, value.type, static_cast<int>(value.name.size()), value.name.data(),
               static_cast<int>(value.detail.size()), value.detail.data(),
               static_cast<long long>(value.bid), value.bid_size,
               static_cast<long long>(value.ask), value.ask_size, value.bid_levels,
               value.ask_levels, static_cast<unsigned long long>(value.resume_from),
               value.matches ? "true" : "false");
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_GLIMPSE_TRACE_HPP

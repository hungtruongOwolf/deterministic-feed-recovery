// IEX DEEP 1.0: the message layer, and where its field offsets came from.
//
// Why this exists at all
// ----------------------
// Until now this library decoded the *envelope* and never the letter. MoldUDP64 and IEX-TP frame messages, and
// the messages themselves were opaque bytes — the benchmark literally filled them with `(i + at) & 0xFF`. So
// recovery could prove "every sequence number arrived exactly once" and could not prove anything about what
// arrived. The strongest invariant a feed handler can offer is not about sequence numbers: it is that **the
// book after recovery equals the book that would have existed with no loss at all**, and that needs the
// messages to mean something.
//
// Where these offsets came from, which is the part that matters
// ------------------------------------------------------------
// Not transcribed from a specification. The DEEP specification's live URL serves a stub, the same way IEX-TP's
// does, so a transcription would have exactly one source and no way to check it — the situation
// docs/DESIGN.md calls out as how a decoder ends up confidently wrong.
//
// Instead: a real IEX HIST capture (2017-08-26 DEEP, 20,145 packets, 48,635 messages) was tabulated by message
// type and length before a line of this was written. That gave the eleven types below and their exact sizes as
// *observed facts*. The field layout within each was then confirmed semantically, which is stronger than
// matching a document:
//
//   * every timestamp decodes to 2017-08-26, the capture's own date;
//   * the symbols decode to real tickers — WWE, IEXT, VIAV;
//   * a Price Level Update Buy at $20.8900 and a Sell at $20.9000 on the same symbol at the same instant,
//     which is a valid one-cent spread and would be impossible if the price offset were wrong;
//   * a Trade Report at $20.9000 — the ask — for 100 shares.
//
// A wrong offset cannot produce a coherent spread by accident. That is the evidence, and `tools/inspect
// --deep` reproduces it on any capture.
//
// The lengths are fixed, and the framing does not rely on that
// -----------------------------------------------------------
// Every type has exactly one observed length. But IEX-TP already prefixes each message with its own length, so
// the decoders check the declared length against the type's expected length rather than assuming either. A
// message whose length disagrees with its type is refused: on a feed that has added a field, decoding it
// against the old layout produces a *plausible* wrong price, which is worse than refusing.

#ifndef DFR_WIRE_DEEP_CONSTANTS_HPP
#define DFR_WIRE_DEEP_CONSTANTS_HPP

#include <dfr/core/assert.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace wire::deep {

// The eleven types observed across a whole trading day's capture, with the counts that were seen. The counts
// are here as provenance rather than as anything the code uses: they say which paths real data exercises, and
// `A` at 5,286 versus `B` at 1 is why the Auction Information decoder matters more than Trade Break.
enum class message_type : std::uint8_t {
  // Administrative.
  system_event = 'S',        // 5 seen
  security_directory = 'D',  // 3
  trading_status = 'H',      // 8,895
  operational_halt = 'O',    // 8,445
  short_sale_test = 'P',     // 8,697
  security_event = 'E',      // 16,884 — the most common message in the capture

  // The book.
  price_level_buy = '8',   // 138
  price_level_sell = '5',  // 116

  // Executions.
  trade_report = 'T',  // 165
  trade_break = 'B',   // 1

  // Auctions.
  auction_information = 'A',  // 5,286
};

[[nodiscard]] constexpr std::string_view name_of(message_type value) noexcept {
  switch (value) {
    case message_type::system_event:        return "system_event";
    case message_type::security_directory:  return "security_directory";
    case message_type::trading_status:      return "trading_status";
    case message_type::operational_halt:    return "operational_halt";
    case message_type::short_sale_test:     return "short_sale_test";
    case message_type::security_event:      return "security_event";
    case message_type::price_level_buy:     return "price_level_buy";
    case message_type::price_level_sell:    return "price_level_sell";
    case message_type::trade_report:        return "trade_report";
    case message_type::trade_break:         return "trade_break";
    case message_type::auction_information: return "auction_information";
  }
  DFR_UNREACHABLE("unnamed DEEP message type");
}

// Whether a byte is a type this build knows. Written as a function rather than a range check because the
// values are eleven scattered ASCII characters and a range would silently admit the gaps.
[[nodiscard]] constexpr bool is_known(std::uint8_t byte) noexcept {
  switch (byte) {
    case 'S': case 'D': case 'H': case 'O': case 'P': case 'E':
    case '8': case '5': case 'T': case 'B': case 'A':
      return true;
    default:
      return false;
  }
}

// The observed length of each type, to the byte. A message whose IEX-TP length prefix disagrees with this is
// refused rather than decoded against the wrong layout.
[[nodiscard]] constexpr std::size_t expected_size(message_type value) noexcept {
  switch (value) {
    case message_type::system_event:        return 10;
    case message_type::operational_halt:    return 18;
    case message_type::security_event:      return 18;
    case message_type::short_sale_test:     return 19;
    case message_type::trading_status:      return 22;
    case message_type::price_level_buy:     return 30;
    case message_type::price_level_sell:    return 30;
    case message_type::security_directory:  return 31;
    case message_type::trade_report:        return 38;
    case message_type::trade_break:         return 38;
    case message_type::auction_information: return 80;
  }
  DFR_UNREACHABLE("no expected size for DEEP message type");
}

// ---------------------------------------------------------------------------
// Field offsets
// ---------------------------------------------------------------------------
//
// Every message starts the same way — type, one flag byte, an eight-byte timestamp — and every message except
// System Event follows with an eight-byte symbol. Naming that prefix once rather than per message is what makes
// the decoders short enough to check by eye.

inline constexpr std::size_t kTypeOffset = 0;
inline constexpr std::size_t kFlagsOffset = 1;
inline constexpr std::size_t kTimestampOffset = 2;
inline constexpr std::size_t kSymbolOffset = 10;
inline constexpr std::size_t kSymbolSize = 8;

// Price Level Update, and Trade Report's leading half, share these.
inline constexpr std::size_t kSizeOffset = 18;
inline constexpr std::size_t kPriceOffset = 22;
inline constexpr std::size_t kTradeIdOffset = 30;

// Security Directory.
inline constexpr std::size_t kRoundLotOffset = 18;
inline constexpr std::size_t kAdjustedPocOffset = 22;
inline constexpr std::size_t kLuldTierOffset = 30;

// Trading Status carries a four-character reason after the symbol.
inline constexpr std::size_t kReasonOffset = 18;
inline constexpr std::size_t kReasonSize = 4;

// Short Sale Price Test carries one detail byte after the symbol.
inline constexpr std::size_t kDetailOffset = 18;

// Prices are signed 64-bit fixed point with four implied decimal places: 20.8900 is 208,900.
//
// Signed, and the sign is not decoration — DEEP uses a negative price for "no price" in some auction fields,
// so an unsigned read would turn an absence into an enormous number.
inline constexpr std::int64_t kPriceScale = 10'000;

}  // namespace wire::deep
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_DEEP_CONSTANTS_HPP

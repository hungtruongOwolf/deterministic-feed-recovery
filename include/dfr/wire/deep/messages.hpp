// The DEEP messages, and the decoders that refuse rather than guess.
//
// Structs and decoders together, the way wire/ouch/inbound.hpp does it: a reader checking a field offset wants
// both in front of them, and separating them would mean holding one file's line numbers in your head while
// reading the other.
//
// Everything little-endian. IEX-TP is little-endian throughout: unusually, since most exchange protocols are
// big-endian, and the capture confirms it: a big-endian read of the WWE price gives 0x0403030000000000, which
// is 289 trillion dollars.

#ifndef DFR_WIRE_DEEP_MESSAGES_HPP
#define DFR_WIRE_DEEP_MESSAGES_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/deep/constants.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::deep {

// A price as it is on the wire, and arithmetic that cannot silently lose a fraction.
//
// A separate type from wire::ouch::price on purpose: OUCH's scale is 10^4 with a sentinel for "market", DEEP's
// is 10^4 signed with no sentinel, and sharing one type would mean one of the two carrying a concept it does
// not have. The two are never mixed in one message, so there is nothing to gain by unifying them.
class price {
 public:
  constexpr price() noexcept = default;
  explicit constexpr price(std::int64_t raw) noexcept : raw_(raw) {}

  [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr std::int64_t dollars() const noexcept { return raw_ / kPriceScale; }
  /** The fractional part in ten-thousandths, always positive so a negative price prints correctly. */
  [[nodiscard]] constexpr std::int64_t ten_thousandths() const noexcept {
    const auto part = raw_ % kPriceScale;
    return part < 0 ? -part : part;
  }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }

  [[nodiscard]] friend constexpr bool operator==(price, price) = default;
  [[nodiscard]] friend constexpr auto operator<=>(price, price) = default;

 private:
  std::int64_t raw_{0};
};

// The prefix every message shares. Decoded once so eleven decoders do not each repeat three field reads.
struct header {
  message_type type{message_type::system_event};
  std::uint8_t flags{0};
  /** Nanoseconds since the Unix epoch. On the 2017-08-26 capture every one lands on that date. */
  std::uint64_t timestamp_ns{0};
};

// A symbol as it sits on the wire: eight bytes, space padded on the right.
//
// Returned as a view into the caller's bytes rather than copied. The trailing spaces are trimmed because a
// symbol is compared against "WWE" by every caller and "WWE     " by none, but the *padding* is what the wire
// carries, so a writer must pad and a reader must trim, and doing the trim here means no caller forgets.
// Not `constexpr`, and GCC is the reason it says so out loud.
//
// A `std::string_view` over `std::byte` needs a `reinterpret_cast`, and a reinterpret_cast is **forbidden in
// constant evaluation**. Clang accepted the `constexpr` because nothing ever constant-evaluated it, so the
// invalid path was never instantiated; GCC 14 diagnoses it eagerly and is right. The keyword was a claim the
// function could not honour, and anybody who took it up would have got a hard error rather than a slow function.
//
// Dropping it is the truthful fix. Keeping it would need the bytes to be `char` underneath, which would mean
// `packet_view` giving up `std::byte`, and `std::byte` is what stops a byte being arithmetic by accident.
[[nodiscard]] inline std::string_view trimmed_symbol(packet_view field) noexcept {
  std::size_t length = field.size();
  while (length > 0 && field.u8_at(length - 1) == ' ') {
    --length;
  }
  return std::string_view{reinterpret_cast<const char*>(field.data()), length};
}

// ---------------------------------------------------------------------------
// The messages
// ---------------------------------------------------------------------------

// A price level changed. The two directions are one struct with a `buy` flag, because everything else about
// them is identical and two structs would mean two decoders differing in one line.
struct price_level_update {
  header head{};
  bool buy{false};
  std::string_view symbol{};
  /** Aggregate size at this price. **Zero means the level is gone**: see book/order_book.hpp. */
  std::uint32_t size{0};
  price level{};

  /** §DEEP event flags: bit 0 set means this update completes an event, so a book may publish. */
  [[nodiscard]] constexpr bool event_complete() const noexcept { return (head.flags & 0x01) != 0; }
};

struct trade_report {
  header head{};
  std::string_view symbol{};
  std::uint32_t size{0};
  price at{};
  std::uint64_t trade_id{0};
  /** True for a Trade Break, which cancels a previously reported trade with the same id. */
  bool broken{false};
};

struct trading_status {
  header head{};
  std::string_view symbol{};
  /** 'H' halted, 'O' order acceptance period, 'P' paused, 'T' trading. */
  char status{' '};
  /** Four characters, space padded. Trimmed, so an absent reason is empty rather than "    ". */
  std::string_view reason{};
};

struct security_directory {
  header head{};
  std::string_view symbol{};
  std::uint32_t round_lot{0};
  price adjusted_previous_close{};
  std::uint8_t luld_tier{0};
};

struct system_event {
  header head{};
  /** 'O' start of messages, 'S' start of system hours, 'R' start of regular hours, 'M'/'E'/'C' the closes. */
  char event{' '};
};

// The types this build frames but does not take apart: operational halt, short sale price test, security event,
// auction information. Their lengths are known, so the stream still frames perfectly and a caller still learns
// the symbol and the instant, which is all a book needs from them.
struct other_message {
  header head{};
  std::string_view symbol{};
  /** The whole body, so a caller that wants the rest can decode it without this file growing. */
  packet_view body{};
};

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Reads the shared prefix and checks the length against the type.
//
// The length check is the important half. IEX-TP prefixes each message with its own length, so a message whose
// declared length disagrees with its type is either a feed that has added a field or a corrupted stream; either
// way, decoding it against this layout produces a *plausible* wrong price. Refusing is the only honest answer.
[[nodiscard]] constexpr result<header> decode_header(packet_view message) noexcept {
  if (message.size() < kTimestampOffset + 8) DFR_UNLIKELY {
    return error::truncated_header;
  }
  const auto byte = message.u8_at(kTypeOffset);
  if (!is_known(byte)) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  const auto type = static_cast<message_type>(byte);
  if (message.size() != expected_size(type)) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  return header{.type = type,
                .flags = message.u8_at(kFlagsOffset),
                .timestamp_ns = message.le64_at(kTimestampOffset)};
}

namespace detail {

[[nodiscard]] constexpr result<std::string_view> symbol_of(packet_view message) noexcept {
  packet_view field;
  if (const auto err = message.subview(kSymbolOffset, kSymbolSize).get(field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return trimmed_symbol(field);
}

}  // namespace detail

[[nodiscard]] constexpr result<price_level_update> decode_price_level(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::price_level_buy &&
      head.type != message_type::price_level_sell) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  std::string_view symbol;
  if (const auto err = detail::symbol_of(message).get(symbol); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return price_level_update{
      .head = head,
      .buy = head.type == message_type::price_level_buy,
      .symbol = symbol,
      .size = message.le32_at(kSizeOffset),
      .level = price{static_cast<std::int64_t>(message.le64_at(kPriceOffset))}};
}

[[nodiscard]] constexpr result<trade_report> decode_trade(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::trade_report &&
      head.type != message_type::trade_break) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  std::string_view symbol;
  if (const auto err = detail::symbol_of(message).get(symbol); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return trade_report{.head = head,
                      .symbol = symbol,
                      .size = message.le32_at(kSizeOffset),
                      .at = price{static_cast<std::int64_t>(message.le64_at(kPriceOffset))},
                      .trade_id = message.le64_at(kTradeIdOffset),
                      .broken = head.type == message_type::trade_break};
}

[[nodiscard]] constexpr result<trading_status> decode_trading_status(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::trading_status) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  std::string_view symbol;
  if (const auto err = detail::symbol_of(message).get(symbol); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  packet_view reason_field;
  if (const auto err = message.subview(kReasonOffset, kReasonSize).get(reason_field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return trading_status{.head = head,
                        .symbol = symbol,
                        .status = static_cast<char>(head.flags),
                        .reason = trimmed_symbol(reason_field)};
}

[[nodiscard]] constexpr result<security_directory> decode_directory(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::security_directory) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  std::string_view symbol;
  if (const auto err = detail::symbol_of(message).get(symbol); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return security_directory{
      .head = head,
      .symbol = symbol,
      .round_lot = message.le32_at(kRoundLotOffset),
      .adjusted_previous_close =
          price{static_cast<std::int64_t>(message.le64_at(kAdjustedPocOffset))},
      .luld_tier = message.u8_at(kLuldTierOffset)};
}

[[nodiscard]] constexpr result<system_event> decode_system_event(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::system_event) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  return system_event{.head = head, .event = static_cast<char>(head.flags)};
}

// For the four types this build frames without taking apart. The symbol is read because every one of them has
// it at the same offset, and a caller filtering a feed by symbol needs it from every message or from none.
[[nodiscard]] constexpr result<other_message> decode_other(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  std::string_view symbol;
  if (const auto err = detail::symbol_of(message).get(symbol); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return other_message{.head = head, .symbol = symbol, .body = message};
}

}  // namespace dfr::inline v1::wire::deep
#endif  // DFR_WIRE_DEEP_MESSAGES_HPP

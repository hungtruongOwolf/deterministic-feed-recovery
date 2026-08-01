// Writing DEEP messages.
//
// Exists so the mock venue can publish a feed that *means* something. Until now it published
// `(i + at) & 0xFF`, which was enough to prove every sequence number arrived once and could not prove anything
// about content, and the invariant worth having is about content: the book after loss and repair must equal
// the book that never lost anything.
//
// The encoder is also how the decoder gets a second opinion. The tests decode real capture bytes *and*
// round-trip through this, and the two checks fail for different reasons: a capture test catches a layout that
// disagrees with IEX, a round-trip catches a layout that disagrees with itself. Neither subsumes the other.

#ifndef DFR_WIRE_DEEP_ENCODE_HPP
#define DFR_WIRE_DEEP_ENCODE_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/deep/constants.hpp>
#include <dfr/wire/deep/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::deep {

namespace detail {

// Writes a symbol into eight bytes, space padded.
//
// Refuses a symbol that does not fit rather than truncating: two symbols differing only past the eighth
// character would silently become one, which on a feed means two instruments sharing a book.
[[nodiscard]] constexpr result<void> put_symbol(mutable_packet_view out, std::size_t offset,
                                                std::string_view symbol) noexcept {
  if (symbol.size() > kSymbolSize) DFR_UNLIKELY {
    return error::invalid_argument;
  }
  if (!out.contains(offset, kSymbolSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  for (std::size_t i = 0; i < kSymbolSize; ++i) {
    out.put_u8_at(offset + i,
                  i < symbol.size() ? static_cast<std::uint8_t>(symbol[i]) : std::uint8_t{' '});
  }
  return ok();
}

// The prefix every message shares.
[[nodiscard]] constexpr result<void> put_header(mutable_packet_view out, message_type type,
                                                std::uint8_t flags,
                                                std::uint64_t timestamp_ns) noexcept {
  if (!out.contains(0, expected_size(type))) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(kTypeOffset, static_cast<std::uint8_t>(type));
  out.put_u8_at(kFlagsOffset, flags);
  out.put_le64_at(kTimestampOffset, timestamp_ns);
  return ok();
}

}  // namespace detail

// One price level update. Returns how many bytes it occupied, which is always `expected_size` for its type:
// returned anyway so a caller writing a stream advances by what was written rather than by what it assumed.
[[nodiscard]] constexpr result<std::size_t> encode_price_level(
    mutable_packet_view out, bool buy, std::string_view symbol, std::uint32_t size, price level,
    std::uint64_t timestamp_ns, bool event_complete = true) noexcept {
  const auto type = buy ? message_type::price_level_buy : message_type::price_level_sell;
  if (const auto err = detail::put_header(out, type, event_complete ? 0x01 : 0x00, timestamp_ns);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = detail::put_symbol(out, kSymbolOffset, symbol); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_le32_at(kSizeOffset, size);
  out.put_le64_at(kPriceOffset, static_cast<std::uint64_t>(level.raw()));
  return expected_size(type);
}

[[nodiscard]] constexpr result<std::size_t> encode_trade(mutable_packet_view out,
                                                         std::string_view symbol,
                                                         std::uint32_t size, price at,
                                                         std::uint64_t trade_id,
                                                         std::uint64_t timestamp_ns,
                                                         bool broken = false) noexcept {
  const auto type = broken ? message_type::trade_break : message_type::trade_report;
  if (const auto err = detail::put_header(out, type, 0xC0, timestamp_ns); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = detail::put_symbol(out, kSymbolOffset, symbol); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_le32_at(kSizeOffset, size);
  out.put_le64_at(kPriceOffset, static_cast<std::uint64_t>(at.raw()));
  out.put_le64_at(kTradeIdOffset, trade_id);
  return expected_size(type);
}

[[nodiscard]] constexpr result<std::size_t> encode_system_event(mutable_packet_view out, char event,
                                                                std::uint64_t timestamp_ns) noexcept {
  if (const auto err = detail::put_header(out, message_type::system_event,
                                          static_cast<std::uint8_t>(event), timestamp_ns);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  return expected_size(message_type::system_event);
}

[[nodiscard]] constexpr result<std::size_t> encode_trading_status(mutable_packet_view out,
                                                                  std::string_view symbol,
                                                                  char status,
                                                                  std::string_view reason,
                                                                  std::uint64_t timestamp_ns) noexcept {
  if (reason.size() > kReasonSize) DFR_UNLIKELY {
    return error::invalid_argument;
  }
  if (const auto err = detail::put_header(out, message_type::trading_status,
                                          static_cast<std::uint8_t>(status), timestamp_ns);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = detail::put_symbol(out, kSymbolOffset, symbol); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  for (std::size_t i = 0; i < kReasonSize; ++i) {
    out.put_u8_at(kReasonOffset + i,
                  i < reason.size() ? static_cast<std::uint8_t>(reason[i]) : std::uint8_t{' '});
  }
  return expected_size(message_type::trading_status);
}

}  // namespace dfr::inline v1::wire::deep
#endif  // DFR_WIRE_DEEP_ENCODE_HPP

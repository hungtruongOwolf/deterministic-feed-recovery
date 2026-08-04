// Encoders for the ITCH order messages used by the deterministic venue stream.

#ifndef DFR_WIRE_ITCH_ENCODE_HPP
#define DFR_WIRE_ITCH_ENCODE_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/itch/constants.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::itch {

struct header_fields {
  std::uint64_t timestamp_ns{0};
  std::uint16_t stock_locate{0};
  std::uint16_t tracking_number{0};
};

namespace detail {

[[nodiscard]] constexpr result<void> put_header(mutable_packet_view out, message_type type,
                                                header_fields fields) noexcept {
  if (!out.contains(0, expected_size(type))) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  if (fields.timestamp_ns >= (std::uint64_t{1} << 48)) DFR_UNLIKELY {
    return error::invalid_argument;
  }
  out.put_u8_at(kTypeOffset, static_cast<std::uint8_t>(type));
  out.put_be16_at(kStockLocateOffset, fields.stock_locate);
  out.put_be16_at(kTrackingNumberOffset, fields.tracking_number);
  for (std::size_t i = 0; i < 6; ++i) {
    const auto shift = static_cast<unsigned>((5 - i) * 8);
    out.put_u8_at(kTimestampOffset + i,
                  static_cast<std::uint8_t>(fields.timestamp_ns >> shift));
  }
  return ok();
}

[[nodiscard]] constexpr result<void> put_stock(mutable_packet_view out,
                                               std::string_view stock) noexcept {
  if (stock.size() > kStockSize) DFR_UNLIKELY {
    return error::invalid_argument;
  }
  for (std::size_t i = 0; i < kStockSize; ++i) {
    out.put_u8_at(kStockOffset + i,
                  i < stock.size() ? static_cast<std::uint8_t>(stock[i])
                                   : std::uint8_t{' '});
  }
  return ok();
}

}  // namespace detail

[[nodiscard]] constexpr result<std::size_t> encode_add_order(
    mutable_packet_view out, header_fields fields, std::uint64_t reference, char side,
    std::uint32_t shares, std::string_view stock, std::uint32_t price) noexcept {
  if (side != 'B' && side != 'S') DFR_UNLIKELY {
    return error::invalid_argument;
  }
  if (const auto err = detail::put_header(out, message_type::add_order, fields); !err) {
    return err.error_code();
  }
  if (const auto err = detail::put_stock(out, stock); !err) {
    return err.error_code();
  }
  out.put_be64_at(kOrderReferenceOffset, reference);
  out.put_u8_at(kSideOffset, static_cast<std::uint8_t>(side));
  out.put_be32_at(kSharesOffset, shares);
  out.put_be32_at(kPriceOffset, price);
  return expected_size(message_type::add_order);
}

[[nodiscard]] constexpr result<std::size_t> encode_order_executed(
    mutable_packet_view out, header_fields fields, std::uint64_t reference,
    std::uint32_t shares, std::uint64_t match_number) noexcept {
  if (const auto err = detail::put_header(out, message_type::order_executed, fields); !err) {
    return err.error_code();
  }
  out.put_be64_at(kOrderReferenceOffset, reference);
  out.put_be32_at(kSideOffset, shares);
  out.put_be64_at(kMatchNumberOffset, match_number);
  return expected_size(message_type::order_executed);
}

[[nodiscard]] constexpr result<std::size_t> encode_order_cancel(
    mutable_packet_view out, header_fields fields, std::uint64_t reference,
    std::uint32_t shares) noexcept {
  if (const auto err = detail::put_header(out, message_type::order_cancel, fields); !err) {
    return err.error_code();
  }
  out.put_be64_at(kOrderReferenceOffset, reference);
  out.put_be32_at(kSideOffset, shares);
  return expected_size(message_type::order_cancel);
}

[[nodiscard]] constexpr result<std::size_t> encode_order_delete(
    mutable_packet_view out, header_fields fields, std::uint64_t reference) noexcept {
  if (const auto err = detail::put_header(out, message_type::order_delete, fields); !err) {
    return err.error_code();
  }
  out.put_be64_at(kOrderReferenceOffset, reference);
  return expected_size(message_type::order_delete);
}

[[nodiscard]] constexpr result<std::size_t> encode_order_replace(
    mutable_packet_view out, header_fields fields, std::uint64_t original_reference,
    std::uint64_t new_reference, std::uint32_t shares, std::uint32_t price) noexcept {
  if (const auto err = detail::put_header(out, message_type::order_replace, fields); !err) {
    return err.error_code();
  }
  out.put_be64_at(kOrderReferenceOffset, original_reference);
  out.put_be64_at(kNewOrderReferenceOffset, new_reference);
  out.put_be32_at(kReplaceSharesOffset, shares);
  out.put_be32_at(kReplacePriceOffset, price);
  return expected_size(message_type::order_replace);
}

}  // namespace dfr::inline v1::wire::itch
#endif  // DFR_WIRE_ITCH_ENCODE_HPP

// Decoding the order lifecycle from Nasdaq TotalView-ITCH 5.0.

#ifndef DFR_WIRE_ITCH_MESSAGES_HPP
#define DFR_WIRE_ITCH_MESSAGES_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/itch/constants.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::wire::itch {

using stock = std::array<char, kStockSize>;

struct header {
  message_type type{message_type::add_order};
  std::uint16_t stock_locate{0};
  std::uint16_t tracking_number{0};
  std::uint64_t timestamp_ns{0};
};

struct add_order {
  header head{};
  std::uint64_t order_reference{0};
  stock symbol{};
  std::uint32_t shares{0};
  std::uint32_t price{0};
  bool buy{false};
};

struct order_executed {
  header head{};
  std::uint64_t order_reference{0};
  std::uint64_t match_number{0};
  std::uint32_t shares{0};
};

struct order_cancel {
  header head{};
  std::uint64_t order_reference{0};
  std::uint32_t shares{0};
};

struct order_delete {
  header head{};
  std::uint64_t order_reference{0};
};

struct order_replace {
  header head{};
  std::uint64_t original_reference{0};
  std::uint64_t new_reference{0};
  std::uint32_t shares{0};
  std::uint32_t price{0};
};

namespace detail {

[[nodiscard]] constexpr stock stock_at(packet_view message) noexcept {
  stock out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<char>(message.u8_at(kStockOffset + i));
  }
  return out;
}

}  // namespace detail

[[nodiscard]] constexpr result<header> decode_header(packet_view message) noexcept {
  if (message.size() < kOrderReferenceOffset) DFR_UNLIKELY {
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
                .stock_locate = message.be16_at(kStockLocateOffset),
                .tracking_number = message.be16_at(kTrackingNumberOffset),
                .timestamp_ns = message.be48_at(kTimestampOffset)};
}

[[nodiscard]] constexpr result<add_order> decode_add_order(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::add_order) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  const auto side = message.u8_at(kSideOffset);
  if (side != 'B' && side != 'S') DFR_UNLIKELY {
    return error::invalid_argument;
  }
  return add_order{.head = head,
                   .order_reference = message.be64_at(kOrderReferenceOffset),
                   .symbol = detail::stock_at(message),
                   .shares = message.be32_at(kSharesOffset),
                   .price = message.be32_at(kPriceOffset),
                   .buy = side == 'B'};
}

[[nodiscard]] constexpr result<order_executed> decode_order_executed(
    packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::order_executed) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  return order_executed{.head = head,
                        .order_reference = message.be64_at(kOrderReferenceOffset),
                        .match_number = message.be64_at(kMatchNumberOffset),
                        .shares = message.be32_at(kSideOffset)};
}

[[nodiscard]] constexpr result<order_cancel> decode_order_cancel(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::order_cancel) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  return order_cancel{.head = head,
                      .order_reference = message.be64_at(kOrderReferenceOffset),
                      .shares = message.be32_at(kSideOffset)};
}

[[nodiscard]] constexpr result<order_delete> decode_order_delete(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::order_delete) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  return order_delete{.head = head,
                      .order_reference = message.be64_at(kOrderReferenceOffset)};
}

[[nodiscard]] constexpr result<order_replace> decode_order_replace(packet_view message) noexcept {
  header head;
  if (const auto err = decode_header(message).get(head); err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (head.type != message_type::order_replace) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  return order_replace{.head = head,
                       .original_reference = message.be64_at(kOrderReferenceOffset),
                       .new_reference = message.be64_at(kNewOrderReferenceOffset),
                       .shares = message.be32_at(kReplaceSharesOffset),
                       .price = message.be32_at(kReplacePriceOffset)};
}

}  // namespace dfr::inline v1::wire::itch
#endif  // DFR_WIRE_ITCH_MESSAGES_HPP

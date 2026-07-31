// Encoding the four inbound messages, so a client can be simulated as well as a host.
//
// Split from inbound.hpp because the two have different audiences: a recovery client decodes what it
// receives, and only a test or a load generator writes what it sends. A reader fixing a decode bug
// never needs this file.

#ifndef DFR_WIRE_OUCH_INBOUND_ENCODE_HPP
#define DFR_WIRE_OUCH_INBOUND_ENCODE_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/ouch/inbound.hpp>
#include <dfr/wire/soupbintcp/ascii.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace wire::ouch {

namespace detail {

[[nodiscard]] constexpr result<void> put_alpha(mutable_packet_view message,
                                              std::size_t at, std::size_t width,
                                              std::string_view text) noexcept {
  mutable_packet_view field;
  if (const auto err = message.subview(at, width).get(field); err != error::ok)
      DFR_UNLIKELY {
    return err;
  }
  return soupbintcp::put_text_left_justified(field, text);
}

[[nodiscard]] constexpr result<void> put_token(mutable_packet_view message,
                                              std::size_t at,
                                              const order_token& token) noexcept {
  mutable_packet_view field;
  if (const auto err = message.subview(at, kTokenSize).get(field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  token.write_to(field);
  return ok();
}

}  // namespace detail

[[nodiscard]] constexpr result<std::size_t> encode_enter_order(
    mutable_packet_view out, const enter_order& message) noexcept {
  if (!out.contains(0, enter_order_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(inbound_type::enter_order));
  if (const auto err = detail::put_token(out, enter_order_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_u8_at(enter_order_at::kSide, static_cast<std::uint8_t>(message.order_side));
  out.put_be32_at(enter_order_at::kShares, message.shares);
  if (const auto err =
          detail::put_alpha(out, enter_order_at::kStock, kStockSize, message.stock);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(enter_order_at::kPrice, message.limit.raw());
  out.put_be32_at(enter_order_at::kTimeInForce, message.time_in_force);
  if (const auto err =
          detail::put_alpha(out, enter_order_at::kFirm, kFirmSize, message.firm);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_u8_at(enter_order_at::kDisplay, message.display);
  out.put_u8_at(enter_order_at::kCapacity,
                static_cast<std::uint8_t>(message.account_capacity));
  out.put_u8_at(enter_order_at::kSweepEligibility,
                static_cast<std::uint8_t>(message.sweep));
  out.put_be32_at(enter_order_at::kMinimumQuantity, message.minimum_quantity);
  out.put_u8_at(enter_order_at::kCrossType, message.cross_type);
  out.put_u8_at(enter_order_at::kCustomerType, message.customer_type);
  return enter_order_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_replace_order(
    mutable_packet_view out, const replace_order& message) noexcept {
  if (!out.contains(0, replace_order_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(inbound_type::replace_order));
  if (const auto err = detail::put_token(out, replace_order_at::kExistingToken,
                                         message.existing_token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = detail::put_token(out, replace_order_at::kReplacementToken,
                                         message.replacement_token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(replace_order_at::kShares, message.total_shares_liable);
  out.put_be32_at(replace_order_at::kPrice, message.limit.raw());
  out.put_be32_at(replace_order_at::kTimeInForce, message.time_in_force);
  out.put_u8_at(replace_order_at::kDisplay, message.display);
  out.put_u8_at(replace_order_at::kSweepEligibility,
                static_cast<std::uint8_t>(message.sweep));
  out.put_be32_at(replace_order_at::kMinimumQuantity, message.minimum_quantity);
  return replace_order_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_cancel_order(
    mutable_packet_view out, const cancel_order& message) noexcept {
  if (!out.contains(0, cancel_order_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(inbound_type::cancel_order));
  if (const auto err = detail::put_token(out, cancel_order_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(cancel_order_at::kShares, message.intended_order_size);
  return cancel_order_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_modify_order(
    mutable_packet_view out, const modify_order& message) noexcept {
  if (!out.contains(0, modify_order_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(inbound_type::modify_order));
  if (const auto err = detail::put_token(out, modify_order_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_u8_at(modify_order_at::kSide,
                static_cast<std::uint8_t>(message.order_side));
  out.put_be32_at(modify_order_at::kShares, message.total_shares_liable);
  return modify_order_at::kSize;
}

}  // namespace wire::ouch
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_OUCH_INBOUND_ENCODE_HPP

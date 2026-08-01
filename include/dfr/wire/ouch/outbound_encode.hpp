// Encoding what a host sends. This is the file that makes dfr::venue an exchange rather than a stub.
//
// Per RESEARCH-DOSSIER.md the gap this project targets is exactly here: ITCH and OUCH *decoders* are
// saturated, while encoders that behave like an exchange number roughly zero. Everybody builds the
// ingress side, because that is what a trading system needs; nobody builds the thing to test it
// against.

#ifndef DFR_WIRE_OUCH_OUTBOUND_ENCODE_HPP
#define DFR_WIRE_OUCH_OUTBOUND_ENCODE_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/ouch/inbound_encode.hpp>
#include <dfr/wire/ouch/outbound_ack.hpp>
#include <dfr/wire/ouch/outbound_events.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::wire::ouch {

namespace detail {

// Writes the fields both acknowledgements share, at whichever offsets that message uses.
[[nodiscard]] constexpr result<void> put_ack(mutable_packet_view out,
                                           const ack_offsets& at,
                                           const acknowledged_order& order) noexcept {
  out.put_be64_at(at.timestamp, order.timestamp_ns);
  out.put_u8_at(at.side, static_cast<std::uint8_t>(order.order_side));
  if (const auto err = put_alpha(out, at.stock, kStockSize, order.stock); !err)
      DFR_UNLIKELY {
    return err;
  }
  out.put_be32_at(at.price, order.limit.raw());
  out.put_be32_at(at.time_in_force, order.time_in_force);
  if (const auto err = put_alpha(out, at.firm, kFirmSize, order.firm); !err)
      DFR_UNLIKELY {
    return err;
  }
  out.put_u8_at(at.display, order.display);
  out.put_be64_at(at.reference_number, order.reference_number);
  out.put_u8_at(at.capacity, static_cast<std::uint8_t>(order.account_capacity));
  out.put_u8_at(at.sweep, static_cast<std::uint8_t>(order.sweep));
  out.put_be32_at(at.minimum_quantity, order.minimum_quantity);
  out.put_u8_at(at.cross_type, order.cross_type);
  out.put_u8_at(at.order_state, static_cast<std::uint8_t>(order.state));
  out.put_u8_at(at.bbo_weight, order.bbo_weight);
  return ok();
}

}  // namespace detail

[[nodiscard]] constexpr result<std::size_t> encode_system_event(
    mutable_packet_view out, const system_event& message) noexcept {
  if (!out.contains(0, system_event_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::system_event));
  out.put_be64_at(system_event_at::kTimestamp, message.timestamp_ns);
  out.put_u8_at(system_event_at::kEventCode,
                static_cast<std::uint8_t>(message.code));
  return system_event_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_accepted(
    mutable_packet_view out, const accepted& message) noexcept {
  if (!out.contains(0, accepted_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::accepted));
  if (const auto err = detail::put_token(out, accepted_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(accepted_at::kShares, message.shares_accepted);
  if (const auto err =
          detail::put_ack(out, detail::kAcceptedOffsets, message.order);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  return accepted_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_replaced(
    mutable_packet_view out, const replaced& message) noexcept {
  if (!out.contains(0, replaced_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::replaced));
  if (const auto err = detail::put_token(out, replaced_at::kReplacementToken,
                                         message.replacement_token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err =
          detail::put_token(out, replaced_at::kPreviousToken, message.previous_token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(replaced_at::kShares, message.shares_outstanding);
  if (const auto err =
          detail::put_ack(out, detail::kReplacedOffsets, message.order);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  return replaced_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_executed(
    mutable_packet_view out, const executed& message) noexcept {
  if (!out.contains(0, executed_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::executed));
  out.put_be64_at(executed_at::kTimestamp, message.timestamp_ns);
  if (const auto err = detail::put_token(out, executed_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(executed_at::kExecutedShares, message.shares_this_fill);
  out.put_be32_at(executed_at::kExecutionPrice, message.execution_price.raw());
  out.put_u8_at(executed_at::kLiquidityFlag, message.liquidity_flag.byte());
  out.put_be64_at(executed_at::kMatchNumber, message.match_number);
  return executed_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_canceled(
    mutable_packet_view out, const canceled& message) noexcept {
  if (!out.contains(0, canceled_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::canceled));
  out.put_be64_at(canceled_at::kTimestamp, message.timestamp_ns);
  if (const auto err = detail::put_token(out, canceled_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(canceled_at::kDecrementShares, message.shares_decremented);
  out.put_u8_at(canceled_at::kReason, message.reason.byte());
  return canceled_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_rejected(
    mutable_packet_view out, const rejected& message) noexcept {
  if (!out.contains(0, rejected_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::rejected));
  out.put_be64_at(rejected_at::kTimestamp, message.timestamp_ns);
  if (const auto err = detail::put_token(out, rejected_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_u8_at(rejected_at::kReason, message.reason.byte());
  return rejected_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_broken_trade(
    mutable_packet_view out, const broken_trade& message) noexcept {
  if (!out.contains(0, broken_trade_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::broken_trade));
  out.put_be64_at(broken_trade_at::kTimestamp, message.timestamp_ns);
  if (const auto err = detail::put_token(out, broken_trade_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be64_at(broken_trade_at::kMatchNumber, message.match_number);
  out.put_u8_at(broken_trade_at::kReason, message.reason.byte());
  return broken_trade_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_cancel_notice(
    mutable_packet_view out, const cancel_notice& message) noexcept {
  if (!out.contains(0, cancel_notice_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(message.pending
                                                 ? outbound_type::cancel_pending
                                                 : outbound_type::cancel_reject));
  out.put_be64_at(cancel_notice_at::kTimestamp, message.timestamp_ns);
  if (const auto err = detail::put_token(out, cancel_notice_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  return cancel_notice_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_modified(
    mutable_packet_view out, const modified& message) noexcept {
  if (!out.contains(0, modified_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::modified));
  out.put_be64_at(modified_at::kTimestamp, message.timestamp_ns);
  if (const auto err = detail::put_token(out, modified_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_u8_at(modified_at::kSide, static_cast<std::uint8_t>(message.order_side));
  out.put_be32_at(modified_at::kShares, message.shares_outstanding);
  return modified_at::kSize;
}

[[nodiscard]] constexpr result<std::size_t> encode_priority_update(
    mutable_packet_view out, const priority_update& message) noexcept {
  if (!out.contains(0, priority_update_at::kSize)) DFR_UNLIKELY {
    return error::capacity_exceeded;
  }
  out.put_u8_at(0, static_cast<std::uint8_t>(outbound_type::priority_update));
  out.put_be64_at(priority_update_at::kTimestamp, message.timestamp_ns);
  if (const auto err =
          detail::put_token(out, priority_update_at::kToken, message.token);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  out.put_be32_at(priority_update_at::kPrice, message.limit.raw());
  out.put_u8_at(priority_update_at::kDisplay, message.display);
  out.put_be64_at(priority_update_at::kReferenceNumber, message.reference_number);
  return priority_update_at::kSize;
}

}  // namespace dfr::inline v1::wire::ouch
#endif  // DFR_WIRE_OUCH_OUTBOUND_ENCODE_HPP

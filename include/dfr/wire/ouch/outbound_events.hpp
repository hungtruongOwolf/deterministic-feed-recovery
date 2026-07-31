// Everything else the host sends: what happened to an order after it was acknowledged.
//
// Two counting rules that are easy to invert
// -----------------------------------------
// Executed Shares is **incremental** — the shares of *this* fill, not the running total. Canceled's
// Decrement Shares likewise: §3.5, "This number is incremental, not cumulative." So a client tracking
// its position accumulates them, and one that assigns them overwrites its own history: three fills of
// 100 on a 300-share order would read as an order that is still 200 short.
//
// The fields are named `shares_this_fill` and `shares_decremented` for that reason.
//
// A cancel has no failure acknowledgement
// --------------------------------------
// §2.3: "the only acknowledgement to a Cancel Order Message is the resulting Canceled Order Message.
// There is no 'too late to cancel' message since by the time you received it, you would already have
// gotten the execution. Superfluous Cancel Order Messages are silently ignored."
//
// So silence after a cancel is not a lost message — it is the exchange telling you the order was
// already gone. A client that retried on silence would generate cancels forever, and a simulator that
// answered them would be modelling an exchange that does not exist.

#ifndef DFR_WIRE_OUCH_OUTBOUND_EVENTS_HPP
#define DFR_WIRE_OUCH_OUTBOUND_EVENTS_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/ouch/constants.hpp>
#include <dfr/wire/ouch/enums.hpp>
#include <dfr/wire/ouch/inbound.hpp>
#include <dfr/wire/ouch/price.hpp>
#include <dfr/wire/ouch/token.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace wire::ouch {

struct system_event {
  std::uint64_t timestamp_ns{0};
  event_code code{event_code::start_of_day};
};

struct executed {
  std::uint64_t timestamp_ns{0};
  order_token token{};
  // Incremental: the shares of this fill alone.
  std::uint32_t shares_this_fill{0};
  price execution_price{};
  reason_code liquidity_flag{};
  // "Each match consists of one buy and one sell. The matching buy and sell executions share the same
  // match number" — so it identifies the trade, not the order, and it is what a Broken Trade refers to.
  std::uint64_t match_number{0};
};

struct canceled {
  std::uint64_t timestamp_ns{0};
  order_token token{};
  // Incremental, and a cancel does not necessarily mean the order is dead: §3.5, "some portion of the
  // order may still be alive."
  std::uint32_t shares_decremented{0};
  reason_code reason{};
};

struct rejected {
  std::uint64_t timestamp_ns{0};
  order_token token{};
  reason_code reason{};
};

struct broken_trade {
  std::uint64_t timestamp_ns{0};
  order_token token{};
  // The match being broken. §3.8: an Executed message always precedes this for the same match, so a
  // client that has no record of the match number has lost one.
  std::uint64_t match_number{0};
  reason_code reason{};
};

// Cancel Pending and Cancel Reject carry the same two fields. Kept as one type with the kind attached,
// because a caller does the same thing with both — stop waiting — and differs only in what it may do
// next.
struct cancel_notice {
  std::uint64_t timestamp_ns{0};
  order_token token{};
  // Pending: the cancel could not be applied now but will be applied automatically after the cross.
  // Reject: it could not be applied and nothing is scheduled, so the client may ask again later.
  bool pending{true};
};

struct modified {
  std::uint64_t timestamp_ns{0};
  order_token token{};
  side order_side{side::sell};
  std::uint32_t shares_outstanding{0};
};

struct priority_update {
  std::uint64_t timestamp_ns{0};
  order_token token{};
  price limit{};
  std::uint8_t display{'Y'};
  // A new reference number is assigned, because the priority change makes it a different position in
  // the book. A client keyed on reference number rather than token loses the order here.
  std::uint64_t reference_number{0};
};

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] constexpr result<void> check(packet_view message, std::size_t size,
                                          outbound_type type) noexcept {
  if (message.size() != size) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(0) != static_cast<std::uint8_t>(type)) DFR_UNLIKELY {
    return error::unknown_message_type;
  }
  return ok();
}

}  // namespace detail

[[nodiscard]] constexpr result<system_event> decode_system_event(
    packet_view message) noexcept {
  if (const auto err = detail::check(message, system_event_at::kSize,
                                     outbound_type::system_event);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  return system_event{
      .timestamp_ns = message.be64_at(system_event_at::kTimestamp),
      .code = static_cast<event_code>(message.u8_at(system_event_at::kEventCode))};
}

[[nodiscard]] constexpr result<executed> decode_executed(
    packet_view message) noexcept {
  if (const auto err =
          detail::check(message, executed_at::kSize, outbound_type::executed);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  executed out;
  out.timestamp_ns = message.be64_at(executed_at::kTimestamp);
  if (const auto err = detail::token_at(message, executed_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.shares_this_fill = message.be32_at(executed_at::kExecutedShares);
  out.execution_price = price::from_raw(message.be32_at(executed_at::kExecutionPrice));
  out.liquidity_flag = reason_code{message.u8_at(executed_at::kLiquidityFlag)};
  out.match_number = message.be64_at(executed_at::kMatchNumber);
  return out;
}

[[nodiscard]] constexpr result<canceled> decode_canceled(
    packet_view message) noexcept {
  if (const auto err =
          detail::check(message, canceled_at::kSize, outbound_type::canceled);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  canceled out;
  out.timestamp_ns = message.be64_at(canceled_at::kTimestamp);
  if (const auto err = detail::token_at(message, canceled_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.shares_decremented = message.be32_at(canceled_at::kDecrementShares);
  out.reason = reason_code{message.u8_at(canceled_at::kReason)};
  return out;
}

[[nodiscard]] constexpr result<rejected> decode_rejected(
    packet_view message) noexcept {
  if (const auto err =
          detail::check(message, rejected_at::kSize, outbound_type::rejected);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  rejected out;
  out.timestamp_ns = message.be64_at(rejected_at::kTimestamp);
  if (const auto err = detail::token_at(message, rejected_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.reason = reason_code{message.u8_at(rejected_at::kReason)};
  return out;
}

[[nodiscard]] constexpr result<broken_trade> decode_broken_trade(
    packet_view message) noexcept {
  if (const auto err = detail::check(message, broken_trade_at::kSize,
                                     outbound_type::broken_trade);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  broken_trade out;
  out.timestamp_ns = message.be64_at(broken_trade_at::kTimestamp);
  if (const auto err = detail::token_at(message, broken_trade_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.match_number = message.be64_at(broken_trade_at::kMatchNumber);
  out.reason = reason_code{message.u8_at(broken_trade_at::kReason)};
  return out;
}

[[nodiscard]] constexpr result<cancel_notice> decode_cancel_notice(
    packet_view message) noexcept {
  const bool pending =
      message.size() > 0 &&
      message.u8_at(0) == static_cast<std::uint8_t>(outbound_type::cancel_pending);
  if (const auto err = detail::check(
          message, cancel_notice_at::kSize,
          pending ? outbound_type::cancel_pending : outbound_type::cancel_reject);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  cancel_notice out;
  out.timestamp_ns = message.be64_at(cancel_notice_at::kTimestamp);
  if (const auto err = detail::token_at(message, cancel_notice_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.pending = pending;
  return out;
}

[[nodiscard]] constexpr result<modified> decode_modified(
    packet_view message) noexcept {
  if (const auto err =
          detail::check(message, modified_at::kSize, outbound_type::modified);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  modified out;
  out.timestamp_ns = message.be64_at(modified_at::kTimestamp);
  if (const auto err = detail::token_at(message, modified_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.order_side = static_cast<side>(message.u8_at(modified_at::kSide));
  out.shares_outstanding = message.be32_at(modified_at::kShares);
  return out;
}

[[nodiscard]] constexpr result<priority_update> decode_priority_update(
    packet_view message) noexcept {
  if (const auto err = detail::check(message, priority_update_at::kSize,
                                     outbound_type::priority_update);
      !err) DFR_UNLIKELY {
    return err.error_code();
  }
  priority_update out;
  out.timestamp_ns = message.be64_at(priority_update_at::kTimestamp);
  if (const auto err =
          detail::token_at(message, priority_update_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.limit = price::from_raw(message.be32_at(priority_update_at::kPrice));
  out.display = message.u8_at(priority_update_at::kDisplay);
  out.reference_number = message.be64_at(priority_update_at::kReferenceNumber);
  return out;
}

}  // namespace wire::ouch
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_OUCH_OUTBOUND_EVENTS_HPP

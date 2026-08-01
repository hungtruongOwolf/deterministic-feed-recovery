// The two acknowledgements that echo an order back: Accepted and Replaced.
//
// They share thirteen fields and differ in one that matters
// -------------------------------------------------------
// Accepted's Shares is "Total number of shares accepted". Replaced's Shares is "Total number of shares
// outstanding", and §3.4 gives the example that makes the difference concrete: enter 500, execute 100,
// replace with 500, and the Replaced message reports **400**, because that is what is left exposed.
//
// The specification then gives the same scenario with the execution *in flight*: enter 500, accept 500,
// replace 500, execute 100 on the original, replaced with 400. The client sent the replace before it
// knew about the execution, and the exchange applied both. That is the order-entry equivalent of the
// Glimpse race(a request crossing a state change) and a client that assumed its replace would report
// the number it asked for would be carrying a hundred shares it did not know about.
//
// So the shared fields live in one struct and the differing one is named for its meaning in each. A
// single struct with a field called `shares` would erase precisely the distinction that costs money.

#ifndef DFR_WIRE_OUCH_OUTBOUND_ACK_HPP
#define DFR_WIRE_OUCH_OUTBOUND_ACK_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/ouch/constants.hpp>
#include <dfr/wire/ouch/enums.hpp>
#include <dfr/wire/ouch/inbound.hpp>
#include <dfr/wire/ouch/price.hpp>
#include <dfr/wire/ouch/token.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::ouch {

// What both acknowledgements say about the order, with the same meaning in each.
//
// Every one of these is the value the *exchange* accepted, which may differ from what was sent: §3.3
// notes the accepted price "will always be better than or equal to the entered price" and the accepted
// Time in Force "equal to or shorter in scope". A client that assumed its own values were echoed back
// would have a stale copy of its own order.
struct acknowledged_order {
  std::uint64_t timestamp_ns{0};
  side order_side{side::buy};
  std::string_view stock{};
  price limit{};
  std::uint32_t time_in_force{0};
  std::string_view firm{};
  std::uint8_t display{'A'};

  // Assigned by the exchange and day-unique. This is the exchange's identifier for the order, as
  // distinct from the token, which is the client's, and a replace gets a *new* reference number.
  std::uint64_t reference_number{0};

  capacity account_capacity{capacity::other};
  sweep_eligibility sweep{sweep_eligibility::not_eligible};
  std::uint32_t minimum_quantity{0};
  std::uint8_t cross_type{'N'};

  // Dead here means accepted and then immediately canceled, with no further messages to come. It is
  // not a rejection, and reporting it as one would have a client retry an order the exchange took.
  order_state state{order_state::live};

  std::uint8_t bbo_weight{' '};
};

struct accepted {
  order_token token{};
  // "Total number of shares accepted."
  std::uint32_t shares_accepted{0};
  acknowledged_order order{};
};

struct replaced {
  order_token replacement_token{};
  // "The Order Token of the order that was replaced."
  order_token previous_token{};
  // "Total number of shares outstanding": what is left exposed once the replacement completed, which
  // is not what the replace asked for if anything executed in the meantime.
  std::uint32_t shares_outstanding{0};
  acknowledged_order order{};
};

namespace detail {

// Reads the fields common to both, given where each message puts them. The offsets are passed rather
// than assumed because the two messages agree on them only up to Order State, and relying on that
// agreement is how a change to one silently corrupts the other.
// Deliberately without default member initialisers, unlike every other aggregate in this library.
//
// Omitting one of these is a bug: a zero offset would silently read the wrong bytes rather than fail. So
// -Wmissing-field-initializers is wanted here, and giving the fields defaults would switch off the one
// check that catches a mistranscribed table.
struct ack_offsets {
  std::size_t timestamp;
  std::size_t side;
  std::size_t stock;
  std::size_t price;
  std::size_t time_in_force;
  std::size_t firm;
  std::size_t display;
  std::size_t reference_number;
  std::size_t capacity;
  std::size_t sweep;
  std::size_t minimum_quantity;
  std::size_t cross_type;
  std::size_t order_state;
  std::size_t bbo_weight;
};

[[nodiscard]] constexpr result<acknowledged_order> read_ack(
    packet_view message, const ack_offsets& at) noexcept {
  acknowledged_order out;
  out.timestamp_ns = message.be64_at(at.timestamp);
  out.order_side = static_cast<side>(message.u8_at(at.side));
  if (const auto err = alpha_at(message, at.stock, kStockSize).get(out.stock);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.limit = price::from_raw(message.be32_at(at.price));
  out.time_in_force = message.be32_at(at.time_in_force);
  if (const auto err = alpha_at(message, at.firm, kFirmSize).get(out.firm);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.display = message.u8_at(at.display);
  out.reference_number = message.be64_at(at.reference_number);
  out.account_capacity = capacity_from_wire(message.u8_at(at.capacity));
  out.sweep = static_cast<sweep_eligibility>(message.u8_at(at.sweep));
  out.minimum_quantity = message.be32_at(at.minimum_quantity);
  out.cross_type = message.u8_at(at.cross_type);
  out.state = static_cast<order_state>(message.u8_at(at.order_state));
  out.bbo_weight = message.u8_at(at.bbo_weight);
  return out;
}

inline constexpr ack_offsets kAcceptedOffsets{
    .timestamp = accepted_at::kTimestamp,
    .side = accepted_at::kSide,
    .stock = accepted_at::kStock,
    .price = accepted_at::kPrice,
    .time_in_force = accepted_at::kTimeInForce,
    .firm = accepted_at::kFirm,
    .display = accepted_at::kDisplay,
    .reference_number = accepted_at::kReferenceNumber,
    .capacity = accepted_at::kCapacity,
    .sweep = accepted_at::kSweepEligibility,
    .minimum_quantity = accepted_at::kMinimumQuantity,
    .cross_type = accepted_at::kCrossType,
    .order_state = accepted_at::kOrderState,
    .bbo_weight = accepted_at::kBboWeight};

inline constexpr ack_offsets kReplacedOffsets{
    .timestamp = replaced_at::kTimestamp,
    .side = replaced_at::kSide,
    .stock = replaced_at::kStock,
    .price = replaced_at::kPrice,
    .time_in_force = replaced_at::kTimeInForce,
    .firm = replaced_at::kFirm,
    .display = replaced_at::kDisplay,
    .reference_number = replaced_at::kReferenceNumber,
    .capacity = replaced_at::kCapacity,
    .sweep = replaced_at::kSweepEligibility,
    .minimum_quantity = replaced_at::kMinimumQuantity,
    .cross_type = replaced_at::kCrossType,
    .order_state = replaced_at::kOrderState,
    .bbo_weight = replaced_at::kBboWeight};

}  // namespace detail

[[nodiscard]] constexpr result<accepted> decode_accepted(
    packet_view message) noexcept {
  if (message.size() != accepted_at::kSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(0) != static_cast<std::uint8_t>(outbound_type::accepted))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }

  accepted out;
  if (const auto err = detail::token_at(message, accepted_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.shares_accepted = message.be32_at(accepted_at::kShares);
  if (const auto err =
          detail::read_ack(message, detail::kAcceptedOffsets).get(out.order);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return out;
}

[[nodiscard]] constexpr result<replaced> decode_replaced(
    packet_view message) noexcept {
  if (message.size() != replaced_at::kSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(0) != static_cast<std::uint8_t>(outbound_type::replaced))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }

  replaced out;
  if (const auto err = detail::token_at(message, replaced_at::kReplacementToken)
                           .get(out.replacement_token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (const auto err =
          detail::token_at(message, replaced_at::kPreviousToken).get(out.previous_token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.shares_outstanding = message.be32_at(replaced_at::kShares);
  if (const auto err =
          detail::read_ack(message, detail::kReplacedOffsets).get(out.order);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return out;
}

}  // namespace dfr::inline v1::wire::ouch
#endif  // DFR_WIRE_OUCH_OUTBOUND_ACK_HPP

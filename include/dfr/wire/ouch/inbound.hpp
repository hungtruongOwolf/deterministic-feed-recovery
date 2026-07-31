// The four messages a client sends: enter, replace, cancel, modify.
//
// The Shares field means something different in each of them, and that is the whole difficulty
// ------------------------------------------------------------------------------------------
// Enter Order: the size of the order. Straightforward.
//
// Replace Order: **the total shares liable for the whole order/replace chain, inclusive of everything
// already executed.** §2.2 spells it out — enter 500, execute 100, and a replace with 500 leaves 400
// exposed while a replace with 600 exposes 500. NASDAQ's stated reason is that it "inhibits the risk of
// double-liability throughout the order/replace chain". An implementation that reads it as "the new
// open quantity" doubles its exposure on every replace that follows a partial fill, and nothing in the
// protocol complains.
//
// Cancel Order: **the new intended order size**, not the number of shares to cancel. §2.3: "This is
// the new intended order size. This limits the maximum number of shares that can be executed in total
// after the cancel is applied. Entering a zero here will cancel any remaining open shares." So a cancel
// with Shares = 100 on a 500-share order is a reduction, and one with Shares = 0 is a full cancel.
// Reading it as "cancel 100 shares" cancels four hundred too few.
//
// Modify Order: the total shares liable, like a replace.
//
// Because a single wrong reading of that field is a money bug rather than a decode error, the fields
// are named for their meaning here — `total_shares_liable`, `intended_order_size` — and not `shares`.

#ifndef DFR_WIRE_OUCH_INBOUND_HPP
#define DFR_WIRE_OUCH_INBOUND_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/ouch/constants.hpp>
#include <dfr/wire/ouch/enums.hpp>
#include <dfr/wire/ouch/price.hpp>
#include <dfr/wire/ouch/token.hpp>
#include <dfr/wire/soupbintcp/ascii.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace wire::ouch {

struct enter_order {
  order_token token{};
  side order_side{side::buy};
  std::uint32_t shares{0};
  std::string_view stock{};
  price limit{};
  std::uint32_t time_in_force{kSystemHours};
  std::string_view firm{};
  std::uint8_t display{'A'};
  capacity account_capacity{capacity::other};
  sweep_eligibility sweep{sweep_eligibility::not_eligible};
  std::uint32_t minimum_quantity{0};
  std::uint8_t cross_type{'N'};
  std::uint8_t customer_type{' '};

  // Whether the exchange would accept it on the numbers alone. Not a substitute for the exchange's
  // answer — a rejection can come for reasons no client can predict — but the checks the specification
  // states outright, so a simulator rejects for the documented reason rather than an invented one.
  [[nodiscard]] constexpr result<void> validate() const noexcept {
    if (!is_valid_share_count(shares)) {
      return error::invalid_argument;
    }
    if (!is_valid_time_in_force(time_in_force)) {
      return error::invalid_argument;
    }
    if (!limit.is_market() && !limit.is_valid_limit()) {
      return error::invalid_argument;
    }
    if (stock.empty() || stock.size() > kStockSize) {
      return error::invalid_argument;
    }
    if (!is_known(order_side)) {
      return error::invalid_argument;
    }
    // A minimum quantity above the order size can never be satisfied, so the order would rest
    // unfillable — reported here rather than left to be discovered as a mystery non-execution.
    if (minimum_quantity > shares) {
      return error::invalid_argument;
    }
    return ok();
  }
};

struct replace_order {
  order_token existing_token{};
  order_token replacement_token{};

  // Cumulative across the chain, inclusive of prior executions. See the note at the top of this file.
  std::uint32_t total_shares_liable{0};

  price limit{};
  std::uint32_t time_in_force{kSystemHours};
  std::uint8_t display{'A'};
  sweep_eligibility sweep{sweep_eligibility::not_eligible};
  std::uint32_t minimum_quantity{0};

  [[nodiscard]] constexpr result<void> validate() const noexcept {
    if (!is_valid_share_count(total_shares_liable)) {
      return error::invalid_argument;
    }
    if (!is_valid_time_in_force(time_in_force)) {
      return error::invalid_argument;
    }
    // §2.2: "replacement Order Tokens may not be the same as Tokens sent in Enter Order Messages" — and
    // in particular a replace onto its own token is not a no-op, it is malformed.
    if (replacement_token == existing_token) {
      return error::invalid_argument;
    }
    return ok();
  }
};

struct cancel_order {
  order_token token{};

  // The new intended order size. Zero cancels everything still open.
  std::uint32_t intended_order_size{0};

  [[nodiscard]] constexpr bool cancels_entirely() const noexcept {
    return intended_order_size == 0;
  }
};

struct modify_order {
  order_token token{};
  side order_side{side::sell};
  std::uint32_t total_shares_liable{0};
};

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] constexpr result<std::string_view> alpha_at(
    packet_view message, std::size_t at, std::size_t width) noexcept {
  packet_view field;
  if (const auto err = message.subview(at, width).get(field); err != error::ok)
      DFR_UNLIKELY {
    return err;
  }
  // Alpha fields are left-justified and padded right with spaces, the same convention SoupBinTCP uses,
  // so the same reader handles them rather than a second one drifting from it.
  return soupbintcp::text_left_justified(field);
}

[[nodiscard]] constexpr result<order_token> token_at(packet_view message,
                                                    std::size_t at) noexcept {
  packet_view field;
  if (const auto err = message.subview(at, kTokenSize).get(field);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return order_token::from_bytes(field);
}

}  // namespace detail

[[nodiscard]] constexpr result<enter_order> decode_enter_order(
    packet_view message) noexcept {
  if (message.size() != enter_order_at::kSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(0) != static_cast<std::uint8_t>(inbound_type::enter_order))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }

  enter_order out;
  if (const auto err = detail::token_at(message, enter_order_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.order_side = static_cast<side>(message.u8_at(enter_order_at::kSide));
  out.shares = message.be32_at(enter_order_at::kShares);
  if (const auto err =
          detail::alpha_at(message, enter_order_at::kStock, kStockSize).get(out.stock);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.limit = price::from_raw(message.be32_at(enter_order_at::kPrice));
  out.time_in_force = message.be32_at(enter_order_at::kTimeInForce);
  if (const auto err =
          detail::alpha_at(message, enter_order_at::kFirm, kFirmSize).get(out.firm);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.display = message.u8_at(enter_order_at::kDisplay);
  out.account_capacity = capacity_from_wire(message.u8_at(enter_order_at::kCapacity));
  out.sweep =
      static_cast<sweep_eligibility>(message.u8_at(enter_order_at::kSweepEligibility));
  out.minimum_quantity = message.be32_at(enter_order_at::kMinimumQuantity);
  out.cross_type = message.u8_at(enter_order_at::kCrossType);
  out.customer_type = message.u8_at(enter_order_at::kCustomerType);
  return out;
}

[[nodiscard]] constexpr result<replace_order> decode_replace_order(
    packet_view message) noexcept {
  if (message.size() != replace_order_at::kSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(0) != static_cast<std::uint8_t>(inbound_type::replace_order))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }

  replace_order out;
  if (const auto err =
          detail::token_at(message, replace_order_at::kExistingToken).get(out.existing_token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  if (const auto err = detail::token_at(message, replace_order_at::kReplacementToken)
                           .get(out.replacement_token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.total_shares_liable = message.be32_at(replace_order_at::kShares);
  out.limit = price::from_raw(message.be32_at(replace_order_at::kPrice));
  out.time_in_force = message.be32_at(replace_order_at::kTimeInForce);
  out.display = message.u8_at(replace_order_at::kDisplay);
  out.sweep =
      static_cast<sweep_eligibility>(message.u8_at(replace_order_at::kSweepEligibility));
  out.minimum_quantity = message.be32_at(replace_order_at::kMinimumQuantity);
  return out;
}

[[nodiscard]] constexpr result<cancel_order> decode_cancel_order(
    packet_view message) noexcept {
  if (message.size() != cancel_order_at::kSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(0) != static_cast<std::uint8_t>(inbound_type::cancel_order))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }

  cancel_order out;
  if (const auto err = detail::token_at(message, cancel_order_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.intended_order_size = message.be32_at(cancel_order_at::kShares);
  return out;
}

[[nodiscard]] constexpr result<modify_order> decode_modify_order(
    packet_view message) noexcept {
  if (message.size() != modify_order_at::kSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  if (message.u8_at(0) != static_cast<std::uint8_t>(inbound_type::modify_order))
      DFR_UNLIKELY {
    return error::unknown_message_type;
  }

  modify_order out;
  if (const auto err = detail::token_at(message, modify_order_at::kToken).get(out.token);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  out.order_side = static_cast<side>(message.u8_at(modify_order_at::kSide));
  out.total_shares_liable = message.be32_at(modify_order_at::kShares);
  return out;
}

}  // namespace wire::ouch
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_OUCH_INBOUND_HPP

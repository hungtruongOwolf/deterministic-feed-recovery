// An OUCH order-entry host: what an exchange does with the four messages a client sends.
//
// There is no matching engine here, and that is a scope decision rather than an omission
// -------------------------------------------------------------------------------------
// Matching is the part everybody builds: 1,071 C++ order books created in seven months, per
// RESEARCH-DOSSIER.md. What is missing from the open-source world is the *protocol* behaviour around it:
// which token is consumed by which outcome, what a replace does to a partially executed order, what
// silence means. So executions arrive through execute(), driven by the caller, and the host's job is to
// keep the accounting straight and emit the right messages.
//
// That also makes the interesting race testable on purpose. A caller can form a replace, let an
// execution land, and then submit the replace, which is exactly the scenario OUCH §3.4 describes and
// the reason a Replaced message reports shares the client did not ask for.
//
// Emits encoded bytes, not structs
// -------------------------------
// Like venue::iextp_publisher, and for the same reason: a host that handed back structs would be tested
// against its own idea of the protocol, while one that writes wire bytes is checked by the decoder. The
// tests decode everything this emits.

#ifndef DFR_VENUE_ORDER_ENTRY_HPP
#define DFR_VENUE_ORDER_ENTRY_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/venue/order_messages.hpp>
#include <dfr/venue/order_outcome.hpp>
#include <dfr/venue/order_record.hpp>
#include <dfr/venue/token_registry.hpp>
#include <dfr/wire/ouch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace venue {

inline constexpr std::size_t kMaxLiveOrders = 512;

template <clock_source Clock>
class order_entry {
 public:
  using time_point = typename Clock::time_point;

  explicit constexpr order_entry(order_entry_options options) noexcept
      : options_(options) {}

  [[nodiscard]] constexpr const order_entry_stats& stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] constexpr std::size_t live_orders() const noexcept { return count_; }
  [[nodiscard]] constexpr const token_registry& tokens() const noexcept
      DFR_LIFETIME_BOUND {
    return tokens_;
  }

  [[nodiscard]] constexpr const order_record* find(
      const wire::ouch::order_token& token) const noexcept DFR_LIFETIME_BOUND {
    const std::size_t at = index_of(token);
    return at < count_ ? &orders_[at] : nullptr;
  }

  // Sums the identity across every order the host holds, so a test can assert it once rather than after
  // each step. Every share ever made liable is executed, canceled, or still open.
  [[nodiscard]] constexpr bool accounts() const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (!orders_[i].accounts()) {
        return false;
      }
    }
    return true;
  }

  // ---- Enter Order -------------------------------------------------------

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> enter(
      const wire::ouch::enter_order& request, time_point now, Emit&& emit) noexcept {
    ++stats_.entered;

    // §2.1: "If you send an Enter Order Message with a previously used Order Token, the new order will
    // be ignored." Silence, not a rejection, because a re-send after a connection loss is the
    // protocol's own recovery mechanism and must not look like an error.
    if (tokens_.is_used(request.token)) {
      ++stats_.ignored_duplicate_token;
      return order_outcome::ignored;
    }

    if (const auto invalid = request.validate(); !invalid) DFR_UNLIKELY {
      // A rejection consumes the token (§3.10: "The Order Token of a Rejected Message cannot be
      // re-used"), so it is consumed before the message goes out.
      if (const auto err = tokens_.consume(request.token); !err) DFR_UNLIKELY {
        return err.error_code();
      }
      return writer_.rejected(request.token, reason_for(request), nanos(now), emit);
    }
    if (count_ >= kMaxLiveOrders) DFR_UNLIKELY {
      if (const auto err = tokens_.consume(request.token); !err) DFR_UNLIKELY {
        return err.error_code();
      }
      return writer_.rejected(request.token, wire::ouch::reason_code{'O'}, nanos(now), emit);
    }
    if (const auto err = tokens_.consume(request.token); !err) DFR_UNLIKELY {
      return err.error_code();
    }

    order_record& order = orders_[count_];
    order = order_record{};
    order.token = request.token;
    order.order_side = request.order_side;
    order.set_stock(request.stock);
    order.limit = request.limit;
    order.time_in_force = request.time_in_force;
    order.display = request.display;
    order.account_capacity = request.account_capacity;
    order.cross_type = request.cross_type;
    order.shares_liable = request.shares;
    order.reference_number = writer_.next_reference();
    order.live = true;
    ++count_;

    // An immediate-or-cancel order with nothing to match against is accepted and immediately dead.
    // §3.3: an Accepted message with Order State "D" means exactly that, and no further messages
    // follow, which is different from a rejection and must not be reported as one.
    const bool dies_at_once = wire::ouch::is_immediate_or_cancel(request.time_in_force);
    if (dies_at_once) {
      order.shares_canceled = order.shares_liable;
      order.live = false;
      stats_.shares_canceled += order.shares_canceled;
    }
    DFR_ASSERT_PARANOID(order.accounts(), "an entered order does not account");

    ++stats_.accepted;
    return writer_.accepted(order, firm_for(request.firm), dies_at_once,
                            nanos(now), emit);
  }

  // ---- Cancel Order ------------------------------------------------------

  // §2.3: the Shares field is "the new intended order size", and zero cancels everything open. The only
  // acknowledgement is the resulting Canceled Message: there is no "too late to cancel", because by
  // the time a client saw one it would already have the execution.
  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> cancel(
      const wire::ouch::cancel_order& request, time_point now, Emit&& emit) noexcept {
    order_record* order = mutable_find(request.token);
    if (order == nullptr || !order->live) {
      ++stats_.ignored_unknown_order;
      return order_outcome::ignored;
    }

    // Cannot reduce below what has already executed: those shares are gone, not open.
    const std::uint32_t floor = order->shares_executed;
    const std::uint32_t intended =
        request.intended_order_size > floor ? request.intended_order_size : floor;
    if (intended >= order->shares_liable) {
      // Nothing to take away. A superfluous cancel is silently ignored.
      ++stats_.ignored_unknown_order;
      return order_outcome::ignored;
    }

    const std::uint32_t removed = order->shares_liable - intended;
    order->shares_canceled += removed;
    stats_.shares_canceled += removed;
    if (order->shares_open() == 0) {
      order->live = false;
    }
    DFR_ASSERT_PARANOID(order->accounts(), "a canceled order does not account");

    ++stats_.canceled;
    return writer_.canceled(order->token, removed, wire::ouch::reason_code{'U'},
                            nanos(now), emit);
  }

  // ---- Replace Order ----------------------------------------------------

  // The four outcomes of §2.2, in the order the specification lists them. The token asymmetry between
  // the second and third is the part worth reading twice: an invalid-details replace leaves the
  // replacement token reusable, and a rejection burns it.
  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> replace(
      const wire::ouch::replace_order& request, time_point now,
      Emit&& emit) noexcept {
    order_record* existing = mutable_find(request.existing_token);

    // (1) The existing order is not live, or the replacement token has been used: silently ignored.
    if (existing == nullptr || !existing->live ||
        tokens_.is_used(request.replacement_token)) {
      ++stats_.ignored_unknown_order;
      return order_outcome::ignored;
    }

    // (3) Live but not cancelable: a cross order in the late period. Checked before the details,
    // because the specification's ordering makes this a Reject regardless of what the replace asked
    // for, and it consumes the replacement token.
    if (existing->is_cross() && options_.late_cross_period) {
      if (const auto err = tokens_.consume(request.replacement_token); !err)
          DFR_UNLIKELY {
        return err.error_code();
      }
      return writer_.rejected(request.replacement_token,
                              wire::ouch::reason_code{'R'}, nanos(now), emit);
    }

    // (2) Live, but the replace details are invalid. The existing order is taken out of the book and the
    // replacement token is *not* consumed, so the client may reuse it.
    const bool details_invalid =
        !request.validate().has_value() ||
        request.total_shares_liable < existing->shares_executed;
    if (details_invalid) {
      const std::uint32_t removed = existing->shares_open();
      existing->shares_canceled += removed;
      existing->live = false;
      stats_.shares_canceled += removed;
      DFR_ASSERT_PARANOID(existing->accounts(), "a canceled existing order does not account");
      ++stats_.canceled;
      if (const auto err = writer_.canceled(existing->token, removed,
                                            wire::ouch::reason_code{'U'},
                                            nanos(now), emit);
          !err) DFR_UNLIKELY {
        return err.error_code();
      }
      return order_outcome::replace_canceled_existing;
    }

    // (4) Live and replaceable.
    if (const auto err = tokens_.consume(request.replacement_token); !err)
        DFR_UNLIKELY {
      return err.error_code();
    }
    if (count_ >= kMaxLiveOrders) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    // The executed count carries forward, because the replace's Shares is cumulative for the chain. The
    // new order's open quantity is therefore what is left after everything already done, which is why
    // a Replaced message can report fewer shares than the replace asked for.
    order_record replacement = *existing;
    replacement.previous_token = existing->token;
    replacement.token = request.replacement_token;
    replacement.shares_liable = request.total_shares_liable;
    replacement.shares_canceled = 0;
    replacement.limit = request.limit;
    replacement.time_in_force = request.time_in_force;
    replacement.display = request.display;
    replacement.reference_number = writer_.next_reference();
    replacement.live = true;

    existing->live = false;
    // The existing order's shares are not "canceled": they moved to the replacement, and counting them
    // as canceled would double the chain's accounting.
    existing->shares_liable = existing->shares_executed;
    existing->shares_canceled = 0;

    orders_[count_] = replacement;
    ++count_;
    DFR_ASSERT_PARANOID(replacement.accounts(), "a replacement does not account");

    ++stats_.replaced;
    return writer_.replaced(orders_[count_ - 1], options_.default_firm,
                            nanos(now), emit);
  }

  // ---- Modify Order -----------------------------------------------------

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> modify(
      const wire::ouch::modify_order& request, time_point now, Emit&& emit) noexcept {
    order_record* order = mutable_find(request.token);
    if (order == nullptr || !order->live) {
      ++stats_.ignored_unknown_order;
      return order_outcome::ignored;
    }
    // §3.4 lists the permitted side transitions and nothing else. The specification does not say what
    // happens to a transition outside them, so it is ignored rather than answered with an invented
    // rejection, and the choice is recorded here rather than left implicit.
    if (!wire::ouch::is_permitted_modify_transition(order->order_side,
                                                    request.order_side)) {
      ++stats_.ignored_unknown_order;
      return order_outcome::ignored;
    }
    if (request.total_shares_liable < order->shares_executed) {
      ++stats_.ignored_unknown_order;
      return order_outcome::ignored;
    }

    order->order_side = request.order_side;
    order->shares_liable = request.total_shares_liable;
    DFR_ASSERT_PARANOID(order->accounts(), "a modified order does not account");

    ++stats_.modified_or_none;
    return writer_.modified(*order, nanos(now), emit);
  }

  // ---- Executions, driven by the caller ---------------------------------

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> execute(
      const wire::ouch::order_token& token, std::uint32_t shares,
      wire::ouch::price at, time_point now, Emit&& emit) noexcept {
    order_record* order = mutable_find(token);
    if (order == nullptr || !order->live || shares == 0) {
      ++stats_.ignored_unknown_order;
      return order_outcome::ignored;
    }
    if (shares > order->shares_open()) DFR_UNLIKELY {
      // An execution larger than what is exposed would break the identity, so it is refused rather
      // than clamped: a venue that silently trimmed a fill would be hiding a defect in whatever
      // produced it.
      return error::invalid_argument;
    }

    order->shares_executed += shares;
    stats_.shares_executed += shares;
    if (order->shares_open() == 0) {
      order->live = false;
    }
    DFR_ASSERT_PARANOID(order->accounts(), "an executed order does not account");

    ++stats_.executed;
    return writer_.executed(order->token, shares, at, nanos(now), emit);
  }

 private:
  [[nodiscard]] constexpr std::size_t index_of(
      const wire::ouch::order_token& token) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      // Newest first: a replace chain reuses most of a record, and the live order is the last one
      // written, so searching backwards finds it in one step in the common case.
      const std::size_t at = count_ - 1 - i;
      if (orders_[at].token == token) {
        return at;
      }
    }
    return count_;
  }

  [[nodiscard]] constexpr order_record* mutable_find(
      const wire::ouch::order_token& token) noexcept {
    const std::size_t at = index_of(token);
    return at < count_ ? &orders_[at] : nullptr;
  }

  // The documented reason for an order the numbers already rule out, so a simulator rejects for a stated
  // cause rather than an invented one.
  [[nodiscard]] static constexpr wire::ouch::reason_code reason_for(
      const wire::ouch::enter_order& request) noexcept {
    if (!wire::ouch::is_valid_share_count(request.shares)) {
      return wire::ouch::reason_code{'Z'};  // shares exceed the configured threshold
    }
    if (!request.limit.is_market() && !request.limit.is_valid_limit()) {
      return wire::ouch::reason_code{'X'};  // invalid price
    }
    if (request.stock.empty()) {
      return wire::ouch::reason_code{'S'};  // invalid stock
    }
    if (request.minimum_quantity > request.shares) {
      return wire::ouch::reason_code{'N'};  // invalid minimum quantity
    }
    return wire::ouch::reason_code{'O'};  // other
  }

  // The firm to stamp, since §2.1 says a blank Firm field means the account's default.
  [[nodiscard]] constexpr std::string_view firm_for(std::string_view sent) const noexcept {
    return sent.empty() ? options_.default_firm : sent;
  }

  [[nodiscard]] static constexpr std::uint64_t nanos(time_point now) noexcept {
    return static_cast<std::uint64_t>(now.time_since_epoch().count());
  }

  order_entry_options options_{};
  order_entry_stats stats_{};
  token_registry tokens_{};
  std::array<order_record, kMaxLiveOrders> orders_{};
  std::size_t count_{0};
  message_writer writer_{};
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_ORDER_ENTRY_HPP

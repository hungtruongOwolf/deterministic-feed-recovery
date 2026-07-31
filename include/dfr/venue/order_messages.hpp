// Building the OUCH messages a host sends.
//
// Split from the state machine because they answer different questions: order_entry.hpp decides *what*
// happened, and this decides how to say it on the wire. A reader checking the token asymmetry never
// needs the field-by-field encoding, and a reader checking a field offset never needs the state machine.
//
// Emits encoded bytes rather than structs, like venue::iextp_publisher and for the same reason: a host
// handing back structs would be tested against its own idea of the protocol, while one writing wire
// bytes is checked by the decoder. The tests decode everything this emits.

#ifndef DFR_VENUE_ORDER_MESSAGES_HPP
#define DFR_VENUE_ORDER_MESSAGES_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/venue/order_outcome.hpp>
#include <dfr/venue/order_record.hpp>
#include <dfr/wire/ouch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace venue {

// Owns the scratch buffer and the two day-unique counters the exchange assigns.
//
// The reference number and the match number are the *exchange's* identifiers, as distinct from the
// token, which is the client's. They live here because they are only ever consumed while writing a
// message, and putting them in the state machine would invite something else to read them.
class message_writer {
 public:
  [[nodiscard]] constexpr std::uint64_t references_assigned() const noexcept {
    return next_reference_;
  }
  [[nodiscard]] constexpr std::uint64_t matches_assigned() const noexcept {
    return next_match_;
  }

  [[nodiscard]] constexpr std::uint64_t next_reference() noexcept {
    return ++next_reference_;
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> rejected(
      const wire::ouch::order_token& token, wire::ouch::reason_code reason,
      std::uint64_t timestamp_ns, Emit&& emit) noexcept {
    const wire::ouch::rejected message{
        .timestamp_ns = timestamp_ns, .token = token, .reason = reason};
    return send(wire::ouch::encode_rejected(scratch(), message), order_outcome::rejected,
                emit);
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> accepted(
      const order_record& order, std::string_view firm, bool dead,
      std::uint64_t timestamp_ns, Emit&& emit) noexcept {
    wire::ouch::accepted message;
    message.token = order.token;
    message.shares_accepted = order.shares_liable;
    fill(message.order, order, firm, timestamp_ns,
         dead ? wire::ouch::order_state::dead : wire::ouch::order_state::live);
    return send(wire::ouch::encode_accepted(scratch(), message), order_outcome::accepted,
                emit);
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> replaced(
      const order_record& order, std::string_view firm, std::uint64_t timestamp_ns,
      Emit&& emit) noexcept {
    wire::ouch::replaced message;
    message.replacement_token = order.token;
    message.previous_token = order.previous_token;
    // What is actually left exposed, which is not what the replace asked for if anything executed while
    // it was in flight. This single line is the race OUCH §3.4 describes.
    message.shares_outstanding = order.shares_open();
    fill(message.order, order, firm, timestamp_ns,
         order.shares_open() > 0 ? wire::ouch::order_state::live
                                 : wire::ouch::order_state::dead);
    return send(wire::ouch::encode_replaced(scratch(), message), order_outcome::replaced,
                emit);
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> canceled(
      const wire::ouch::order_token& token, std::uint32_t removed,
      wire::ouch::reason_code reason, std::uint64_t timestamp_ns,
      Emit&& emit) noexcept {
    const wire::ouch::canceled message{.timestamp_ns = timestamp_ns,
                                       .token = token,
                                       .shares_decremented = removed,
                                       .reason = reason};
    return send(wire::ouch::encode_canceled(scratch(), message), order_outcome::canceled,
                emit);
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> executed(
      const wire::ouch::order_token& token, std::uint32_t shares,
      wire::ouch::price at, std::uint64_t timestamp_ns, Emit&& emit) noexcept {
    const wire::ouch::executed message{.timestamp_ns = timestamp_ns,
                                       .token = token,
                                       .shares_this_fill = shares,
                                       .execution_price = at,
                                       .liquidity_flag = wire::ouch::reason_code{'R'},
                                       .match_number = ++next_match_};
    return send(wire::ouch::encode_executed(scratch(), message), order_outcome::executed,
                emit);
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> modified(
      const order_record& order, std::uint64_t timestamp_ns, Emit&& emit) noexcept {
    const wire::ouch::modified message{.timestamp_ns = timestamp_ns,
                                       .token = order.token,
                                       .order_side = order.order_side,
                                       .shares_outstanding = order.shares_open()};
    return send(wire::ouch::encode_modified(scratch(), message), order_outcome::modified,
                emit);
  }

 private:
  // The thirteen fields both acknowledgements echo, written once. Every one is the value the *exchange*
  // accepted, which OUCH §3.3 notes may differ from what was sent.
  static constexpr void fill(wire::ouch::acknowledged_order& into,
                             const order_record& order, std::string_view firm,
                             std::uint64_t timestamp_ns,
                             wire::ouch::order_state state) noexcept {
    into.timestamp_ns = timestamp_ns;
    into.order_side = order.order_side;
    into.stock = order.stock();
    into.limit = order.limit;
    into.time_in_force = order.time_in_force;
    into.firm = firm;
    into.display = order.display;
    into.reference_number = order.reference_number;
    into.account_capacity = order.account_capacity;
    into.cross_type = order.cross_type;
    into.state = state;
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> send(result<std::size_t> written,
                                                    order_outcome outcome,
                                                    Emit&& emit) noexcept {
    std::size_t size = 0;
    if (const auto err = written.get(size); err != error::ok) DFR_UNLIKELY {
      return err;
    }
    emit(packet_view{buffer_.data(), size});
    return outcome;
  }

  [[nodiscard]] constexpr mutable_packet_view scratch() noexcept {
    return mutable_packet_view{buffer_.data(), buffer_.size()};
  }

  std::array<std::byte, wire::ouch::kMaxMessageBytes> buffer_{};
  std::uint64_t next_reference_{0};
  std::uint64_t next_match_{0};
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_ORDER_MESSAGES_HPP

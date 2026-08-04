// Nasdaq TotalView-ITCH 5.0 order messages used by the order-level recovery oracle.

#ifndef DFR_WIRE_ITCH_CONSTANTS_HPP
#define DFR_WIRE_ITCH_CONSTANTS_HPP

#include <dfr/core/assert.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::itch {

enum class message_type : std::uint8_t {
  add_order = 'A',
  order_executed = 'E',
  order_cancel = 'X',
  order_delete = 'D',
  order_replace = 'U',
};

[[nodiscard]] constexpr bool is_known(std::uint8_t byte) noexcept {
  switch (byte) {
    case 'A': case 'E': case 'X': case 'D': case 'U': return true;
    default: return false;
  }
}

[[nodiscard]] constexpr std::string_view name_of(message_type value) noexcept {
  switch (value) {
    case message_type::add_order:      return "add_order";
    case message_type::order_executed: return "order_executed";
    case message_type::order_cancel:   return "order_cancel";
    case message_type::order_delete:   return "order_delete";
    case message_type::order_replace:  return "order_replace";
  }
  DFR_UNREACHABLE("unnamed ITCH message type");
}

[[nodiscard]] constexpr std::size_t expected_size(message_type value) noexcept {
  switch (value) {
    case message_type::add_order:      return 36;
    case message_type::order_executed: return 31;
    case message_type::order_cancel:   return 23;
    case message_type::order_delete:   return 19;
    case message_type::order_replace:  return 35;
  }
  DFR_UNREACHABLE("no size for ITCH message type");
}

inline constexpr std::size_t kTypeOffset = 0;
inline constexpr std::size_t kStockLocateOffset = 1;
inline constexpr std::size_t kTrackingNumberOffset = 3;
inline constexpr std::size_t kTimestampOffset = 5;
inline constexpr std::size_t kOrderReferenceOffset = 11;
inline constexpr std::size_t kSideOffset = 19;
inline constexpr std::size_t kSharesOffset = 20;
inline constexpr std::size_t kStockOffset = 24;
inline constexpr std::size_t kStockSize = 8;
inline constexpr std::size_t kPriceOffset = 32;
inline constexpr std::size_t kMatchNumberOffset = 23;
inline constexpr std::size_t kNewOrderReferenceOffset = 19;
inline constexpr std::size_t kReplaceSharesOffset = 27;
inline constexpr std::size_t kReplacePriceOffset = 31;

}  // namespace dfr::inline v1::wire::itch
#endif  // DFR_WIRE_ITCH_CONSTANTS_HPP

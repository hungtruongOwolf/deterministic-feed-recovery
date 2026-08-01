// OUCH 4.2 message types, sizes and the shared field rules.
//
// Layout transcribed from the NASDAQ OUCH 4.2 specification (updated October 2025), section by
// section. Integers are unsigned big-endian; alpha fields are left-justified and padded on the right
// with spaces.
//
// A limit this project has to state
// --------------------------------
// The IEX-TP layout in this repository was *verified*: 460,578 real packets, zero chain breaks. This
// one cannot be: NASDAQ publishes no OUCH captures, and a live session needs an exchange relationship.
// So every offset here is a transcription, checked against the specification's own offset column and
// pinned by a static_assert on each message's total length, and it is marked unverified rather than
// presented as equivalent evidence. See the honesty ledger in tools/trace.

#ifndef DFR_WIRE_OUCH_CONSTANTS_HPP
#define DFR_WIRE_OUCH_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace wire::ouch {

// ---------------------------------------------------------------------------
// Field widths shared across messages
// ---------------------------------------------------------------------------

inline constexpr std::size_t kTypeSize = 1;
inline constexpr std::size_t kTimestampSize = 8;
inline constexpr std::size_t kSharesSize = 4;
inline constexpr std::size_t kStockSize = 8;
inline constexpr std::size_t kPriceSize = 4;
inline constexpr std::size_t kTimeInForceSize = 4;
inline constexpr std::size_t kFirmSize = 4;
inline constexpr std::size_t kReferenceNumberSize = 8;
inline constexpr std::size_t kMatchNumberSize = 8;

// ---------------------------------------------------------------------------
// Shares and Time in Force
// ---------------------------------------------------------------------------

// §2.1: "Must be greater than zero and less than 1,000,000". The bound is exclusive, so 999,999 is the
// largest legal order.
inline constexpr std::uint32_t kMaxShares = 999'999;

// §1.2 special Time in Force values. Time in Force is otherwise a count of seconds.
inline constexpr std::uint32_t kImmediateOrCancel = 0;
inline constexpr std::uint32_t kExtendedTradingClose = 99'996;
inline constexpr std::uint32_t kMarketHours = 99'998;
inline constexpr std::uint32_t kSystemHours = 99'999;

// §1.2: "Values larger than 99,999 is considered invalid".
inline constexpr std::uint32_t kMaxTimeInForce = 99'999;

// Immediate-or-cancel is expressed as a *zero* lifetime rather than as a flag, which is worth naming:
// a caller that reads Time in Force as "how long it lives" and defaults it to zero has silently
// entered an IOC order that will never rest on the book.
[[nodiscard]] constexpr bool is_immediate_or_cancel(std::uint32_t tif) noexcept {
  return tif == kImmediateOrCancel;
}

[[nodiscard]] constexpr bool is_valid_time_in_force(std::uint32_t tif) noexcept {
  return tif <= kMaxTimeInForce;
}

[[nodiscard]] constexpr bool is_valid_share_count(std::uint32_t shares) noexcept {
  return shares > 0 && shares <= kMaxShares;
}

// ---------------------------------------------------------------------------
// Inbound message types and layouts (client → host, carried unsequenced)
// ---------------------------------------------------------------------------

enum class inbound_type : std::uint8_t {
  enter_order = 'O',
  replace_order = 'U',
  cancel_order = 'X',
  modify_order = 'M',
};

namespace enter_order_at {
inline constexpr std::size_t kToken = 1;
inline constexpr std::size_t kSide = 15;
inline constexpr std::size_t kShares = 16;
inline constexpr std::size_t kStock = 20;
inline constexpr std::size_t kPrice = 28;
inline constexpr std::size_t kTimeInForce = 32;
inline constexpr std::size_t kFirm = 36;
inline constexpr std::size_t kDisplay = 40;
inline constexpr std::size_t kCapacity = 41;
inline constexpr std::size_t kSweepEligibility = 42;
inline constexpr std::size_t kMinimumQuantity = 43;
inline constexpr std::size_t kCrossType = 47;
inline constexpr std::size_t kCustomerType = 48;
inline constexpr std::size_t kSize = 49;
}  // namespace enter_order_at

// Note the two tokens, and that the second is a *new* one. §2.2: "replacement Order Tokens may not be
// the same as Tokens sent in Enter Order Messages".
namespace replace_order_at {
inline constexpr std::size_t kExistingToken = 1;
inline constexpr std::size_t kReplacementToken = 15;
inline constexpr std::size_t kShares = 29;
inline constexpr std::size_t kPrice = 33;
inline constexpr std::size_t kTimeInForce = 37;
inline constexpr std::size_t kDisplay = 41;
inline constexpr std::size_t kSweepEligibility = 42;
inline constexpr std::size_t kMinimumQuantity = 43;
inline constexpr std::size_t kSize = 47;
}  // namespace replace_order_at

namespace cancel_order_at {
inline constexpr std::size_t kToken = 1;
inline constexpr std::size_t kShares = 15;
inline constexpr std::size_t kSize = 19;
}  // namespace cancel_order_at

namespace modify_order_at {
inline constexpr std::size_t kToken = 1;
inline constexpr std::size_t kSide = 15;
inline constexpr std::size_t kShares = 16;
inline constexpr std::size_t kSize = 20;
}  // namespace modify_order_at

// ---------------------------------------------------------------------------
// Outbound message types and layouts (host → client, carried sequenced)
// ---------------------------------------------------------------------------

enum class outbound_type : std::uint8_t {
  system_event = 'S',
  accepted = 'A',
  replaced = 'U',
  canceled = 'C',
  aiq_canceled = 'D',
  executed = 'E',
  executed_with_reference_price = 'G',
  broken_trade = 'B',
  rejected = 'J',
  cancel_pending = 'P',
  cancel_reject = 'I',
  priority_update = 'T',
  modified = 'M',
};

namespace system_event_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kEventCode = 9;
inline constexpr std::size_t kSize = 10;
}  // namespace system_event_at

namespace accepted_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kSide = 23;
inline constexpr std::size_t kShares = 24;
inline constexpr std::size_t kStock = 28;
inline constexpr std::size_t kPrice = 36;
inline constexpr std::size_t kTimeInForce = 40;
inline constexpr std::size_t kFirm = 44;
inline constexpr std::size_t kDisplay = 48;
inline constexpr std::size_t kReferenceNumber = 49;
inline constexpr std::size_t kCapacity = 57;
inline constexpr std::size_t kSweepEligibility = 58;
inline constexpr std::size_t kMinimumQuantity = 59;
inline constexpr std::size_t kCrossType = 63;
inline constexpr std::size_t kOrderState = 64;
inline constexpr std::size_t kBboWeight = 65;
inline constexpr std::size_t kSize = 66;
}  // namespace accepted_at

// Identical to Accepted up to Order State, then the token of the order that was replaced. The overlap
// is not an invitation to share a decoder: the Shares field means something different (see
// ouch/outbound_ack.hpp) and merging them would hide that.
namespace replaced_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kReplacementToken = 9;
inline constexpr std::size_t kSide = 23;
inline constexpr std::size_t kShares = 24;
inline constexpr std::size_t kStock = 28;
inline constexpr std::size_t kPrice = 36;
inline constexpr std::size_t kTimeInForce = 40;
inline constexpr std::size_t kFirm = 44;
inline constexpr std::size_t kDisplay = 48;
inline constexpr std::size_t kReferenceNumber = 49;
inline constexpr std::size_t kCapacity = 57;
inline constexpr std::size_t kSweepEligibility = 58;
inline constexpr std::size_t kMinimumQuantity = 59;
inline constexpr std::size_t kCrossType = 63;
inline constexpr std::size_t kOrderState = 64;
inline constexpr std::size_t kPreviousToken = 65;
inline constexpr std::size_t kBboWeight = 79;
inline constexpr std::size_t kSize = 80;
}  // namespace replaced_at

namespace canceled_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kDecrementShares = 23;
inline constexpr std::size_t kReason = 27;
inline constexpr std::size_t kSize = 28;
}  // namespace canceled_at

namespace executed_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kExecutedShares = 23;
inline constexpr std::size_t kExecutionPrice = 27;
inline constexpr std::size_t kLiquidityFlag = 31;
inline constexpr std::size_t kMatchNumber = 32;
inline constexpr std::size_t kSize = 40;
}  // namespace executed_at

namespace rejected_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kReason = 23;
inline constexpr std::size_t kSize = 24;
}  // namespace rejected_at

namespace broken_trade_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kMatchNumber = 23;
inline constexpr std::size_t kReason = 31;
inline constexpr std::size_t kSize = 32;
}  // namespace broken_trade_at

// Cancel Pending and Cancel Reject share a layout: timestamp then token, nothing else.
namespace cancel_notice_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kSize = 23;
}  // namespace cancel_notice_at

namespace modified_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kSide = 23;
inline constexpr std::size_t kShares = 24;
inline constexpr std::size_t kSize = 28;
}  // namespace modified_at

namespace priority_update_at {
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken = 9;
inline constexpr std::size_t kPrice = 23;
inline constexpr std::size_t kDisplay = 27;
inline constexpr std::size_t kReferenceNumber = 28;
inline constexpr std::size_t kSize = 36;
}  // namespace priority_update_at

// The sizes are pinned rather than trusted: each is the specification's last offset plus that field's
// width, so a mistranscribed offset shows up here as an arithmetic disagreement instead of as a
// silently misread field at run time.
static_assert(enter_order_at::kSize == enter_order_at::kCustomerType + 1);
static_assert(replace_order_at::kSize == replace_order_at::kMinimumQuantity + kSharesSize);
static_assert(cancel_order_at::kSize == cancel_order_at::kShares + kSharesSize);
static_assert(modify_order_at::kSize == modify_order_at::kShares + kSharesSize);
static_assert(accepted_at::kSize == accepted_at::kBboWeight + 1);
static_assert(replaced_at::kSize == replaced_at::kBboWeight + 1);
static_assert(replaced_at::kPreviousToken + 14 == replaced_at::kBboWeight);
static_assert(canceled_at::kSize == canceled_at::kReason + 1);
static_assert(executed_at::kSize == executed_at::kMatchNumber + kMatchNumberSize);
static_assert(rejected_at::kSize == rejected_at::kReason + 1);
static_assert(broken_trade_at::kSize == broken_trade_at::kReason + 1);
static_assert(cancel_notice_at::kSize == cancel_notice_at::kToken + 14);
static_assert(modified_at::kSize == modified_at::kShares + kSharesSize);
static_assert(priority_update_at::kSize ==
              priority_update_at::kReferenceNumber + kReferenceNumberSize);

// The largest message, so a buffer sized for one fits all of them.
inline constexpr std::size_t kMaxMessageBytes = replaced_at::kSize;

}  // namespace wire::ouch
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_OUCH_CONSTANTS_HPP

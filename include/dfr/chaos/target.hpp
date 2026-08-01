// What a fault injector needs to know about a wire protocol.
//
// The injector itself is protocol-agnostic: it decides *which* packets to damage
// and *how*, and a target says where the fields are. That split is what lets one
// injector drive both MoldUDP64 and IEX-TP, and it is docs/DESIGN.md's decision
// for transport handling: a template policy parameter constrained by a concept,
// monomorphised, with no virtual call in the loop.
//
// A target is a stateless type with static functions rather than an object,
// because there is nothing to configure: the offsets come from the specification.

#ifndef DFR_CHAOS_TARGET_HPP
#define DFR_CHAOS_TARGET_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/iextp/constants.hpp>
#include <dfr/wire/moldudp64/constants.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace chaos {

// Every operation returns a result rather than asserting, because the packet is
// wire data: a real capture contains datagrams too short to hold the field a
// fault wants to rewrite, and that is a condition to report rather than a
// programmer error.
template <typename T>
concept fault_target = requires(mutable_packet_view packet, std::uint64_t delta,
                                std::uint32_t session, int adjustment) {
  { T::kName } -> std::convertible_to<std::string_view>;
  { T::kHeaderSize } -> std::convertible_to<std::size_t>;
  { T::add_to_sequence(packet, delta) } -> std::same_as<result<void>>;
  { T::set_session(packet, session) } -> std::same_as<result<void>>;
  { T::adjust_block_count(packet, adjustment) } -> std::same_as<result<void>>;
  { T::adjust_first_block_length(packet, adjustment) } ->
      std::same_as<result<void>>;
};

// ---------------------------------------------------------------------------
// MoldUDP64
// ---------------------------------------------------------------------------

struct moldudp64_target {
  static constexpr std::string_view kName = "MoldUDP64";
  static constexpr std::size_t kHeaderSize = wire::moldudp64::kHeaderSize;

  [[nodiscard]] static result<void> add_to_sequence(
      mutable_packet_view packet, std::uint64_t delta) noexcept {
    if (!packet.contains(wire::moldudp64::kSequenceOffset,
                         wire::moldudp64::kSequenceSize)) DFR_UNLIKELY {
      return error::truncated_header;
    }
    const std::uint64_t current =
        packet.as_const().be64_at(wire::moldudp64::kSequenceOffset);
    packet.put_be64_at(wire::moldudp64::kSequenceOffset, current + delta);
    return ok();
  }

  // The session is ten bytes of text, so a numeric id is written into the last
  // four and the rest left alone. Enough to make it a *different* session, which
  // is all the fault means, while keeping the field printable.
  [[nodiscard]] static result<void> set_session(mutable_packet_view packet,
                                                std::uint32_t session) noexcept {
    if (!packet.contains(wire::moldudp64::kSessionOffset,
                         wire::moldudp64::kSessionSize)) DFR_UNLIKELY {
      return error::truncated_header;
    }
    packet.put_be32_at(wire::moldudp64::kSessionOffset +
                           wire::moldudp64::kSessionSize - 4,
                       session);
    return ok();
  }

  [[nodiscard]] static result<void> adjust_block_count(
      mutable_packet_view packet, int adjustment) noexcept {
    if (!packet.contains(wire::moldudp64::kMessageCountOffset,
                         wire::moldudp64::kMessageCountSize)) DFR_UNLIKELY {
      return error::truncated_header;
    }
    const std::uint16_t current =
        packet.as_const().be16_at(wire::moldudp64::kMessageCountOffset);

    // Saturating rather than wrapping, and never onto the sentinels. A count that
    // wrapped to 0 or 0xFFFF would turn a "the count is a lie" fault into a
    // heartbeat or an end-of-session, which is a different fault entirely and
    // would make the oracle expect the wrong thing.
    const std::int64_t proposed =
        static_cast<std::int64_t>(current) + adjustment;
    std::uint16_t next = 0;
    if (proposed <= 1) {
      next = 1;
    } else if (proposed >= wire::moldudp64::kEndOfSession - 1) {
      next = wire::moldudp64::kEndOfSession - 1;
    } else {
      next = static_cast<std::uint16_t>(proposed);
    }
    packet.put_be16_at(wire::moldudp64::kMessageCountOffset, next);
    return ok();
  }

  [[nodiscard]] static result<void> adjust_first_block_length(
      mutable_packet_view packet, int adjustment) noexcept {
    if (!packet.contains(kHeaderSize, wire::moldudp64::kMessageLengthSize))
        DFR_UNLIKELY {
      return error::truncated_block;
    }
    const std::uint16_t current = packet.as_const().be16_at(kHeaderSize);
    packet.put_be16_at(kHeaderSize, saturate(current, adjustment));
    return ok();
  }

  [[nodiscard]] static std::uint16_t saturate(std::uint16_t current,
                                              int adjustment) noexcept {
    const std::int64_t proposed =
        static_cast<std::int64_t>(current) + adjustment;
    if (proposed < 0) {
      return 0;
    }
    if (proposed > UINT16_MAX) {
      return UINT16_MAX;
    }
    return static_cast<std::uint16_t>(proposed);
  }
};

static_assert(fault_target<moldudp64_target>);

// ---------------------------------------------------------------------------
// IEX-TP
// ---------------------------------------------------------------------------

struct iextp_target {
  static constexpr std::string_view kName = "IEX-TP";
  static constexpr std::size_t kHeaderSize = wire::iextp::kHeaderSize;

  [[nodiscard]] static result<void> add_to_sequence(
      mutable_packet_view packet, std::uint64_t delta) noexcept {
    if (!packet.contains(wire::iextp::kFirstSequenceOffset,
                         wire::iextp::kFirstSequenceSize)) DFR_UNLIKELY {
      return error::truncated_header;
    }
    const std::uint64_t current =
        packet.as_const().le64_at(wire::iextp::kFirstSequenceOffset);
    packet.put_le64_at(wire::iextp::kFirstSequenceOffset, current + delta);
    return ok();
  }

  [[nodiscard]] static result<void> set_session(mutable_packet_view packet,
                                                std::uint32_t session) noexcept {
    if (!packet.contains(wire::iextp::kSessionIdOffset,
                         wire::iextp::kSessionIdSize)) DFR_UNLIKELY {
      return error::truncated_header;
    }
    packet.put_le32_at(wire::iextp::kSessionIdOffset, session);
    return ok();
  }

  // Adjusts Message Count and leaves Payload Length alone, deliberately.
  //
  // The two are redundant with each other, and that redundancy is precisely what
  // makes this fault worth injecting: a receiver checking only the block walk
  // sees a count that does not match, while one checking only the chain sees
  // nothing. Rewriting both would make the packet self-consistent and therefore
  // uninteresting.
  [[nodiscard]] static result<void> adjust_block_count(
      mutable_packet_view packet, int adjustment) noexcept {
    if (!packet.contains(wire::iextp::kMessageCountOffset,
                         wire::iextp::kMessageCountSize)) DFR_UNLIKELY {
      return error::truncated_header;
    }
    const std::uint16_t current =
        packet.as_const().le16_at(wire::iextp::kMessageCountOffset);
    packet.put_le16_at(wire::iextp::kMessageCountOffset,
                       moldudp64_target::saturate(current, adjustment));
    return ok();
  }

  [[nodiscard]] static result<void> adjust_first_block_length(
      mutable_packet_view packet, int adjustment) noexcept {
    if (!packet.contains(kHeaderSize, wire::iextp::kMessageLengthSize))
        DFR_UNLIKELY {
      return error::truncated_block;
    }
    const std::uint16_t current = packet.as_const().le16_at(kHeaderSize);
    packet.put_le16_at(kHeaderSize,
                       moldudp64_target::saturate(current, adjustment));
    return ok();
  }
};

static_assert(fault_target<iextp_target>);

// The two targets differ in byte order at every field, which is the reason
// byte_order.hpp puts the order in each accessor's name. Pinned here because a
// target that read the wrong way round would still compile and would still
// produce a "corrupted" packet: just not the corruption the schedule asked for.
static_assert(moldudp64_target::kHeaderSize == 20);
static_assert(iextp_target::kHeaderSize == 40);

}  // namespace chaos
}  // namespace dfr::inline v1

#endif  // DFR_CHAOS_TARGET_HPP

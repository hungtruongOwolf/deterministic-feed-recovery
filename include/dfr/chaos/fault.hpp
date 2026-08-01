// The fault vocabulary.
//
// A fault is a POD record, not a strategy object with a virtual apply(). That is
// docs/DESIGN.md's decision table row one, and the reason is not only dispatch
// cost: a record can be *stored*, so a whole fault schedule becomes a batch of
// data that can be generated from a seed, written to a file, diffed between two
// runs, and shrunk by deleting entries. A chain of polymorphic strategy objects
// can do none of those.
//
// Acton's rule applies directly: "where there is one, there are many; try
// looking on the time axis". The many here is the schedule; the time axis is the
// packet index.
//
// Every verb is paired with an undo where one is meaningful, following
// FoundationDB's ISimulator (clogPair/unclogPair, disconnectPair/reconnectPair).
// A fault you cannot lift is a fault you cannot test recovery from.

#ifndef DFR_CHAOS_FAULT_HPP
#define DFR_CHAOS_FAULT_HPP

#include <dfr/core/attributes.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace dfr::inline v1::chaos {

// What to do to a packet.
//
// Ordered so that related operations sit together, because the schedule
// generator selects by range and a reader checking that selection should not have
// to jump around.
enum class fault_op : std::uint8_t {
  none = 0,

  // ---- delivery: the packet's bytes are untouched -----------------------
  drop,       // never arrives
  duplicate,  // arrives twice, back to back
  delay,      // arrives `parameter` packets later than it should

  // ---- corruption: the bytes are altered --------------------------------
  flip_bit,  // one bit at byte `parameter`, bit `detail`
  truncate,  // only the first `parameter` bytes arrive

  // ---- framing lies: a length or count field is rewritten ---------------
  //
  // These are the faults that exist because a real implementation got them
  // wrong. penberg/helix trusts the block count and walks off the buffer
  // (moldudp64.hh:106-115), so producing that packet deliberately is the whole
  // point of this library.
  overstate_block_count,
  understate_block_count,
  overstate_block_length,

  // ---- sequencing: the stream's identity or position is rewritten -------
  rewrite_sequence,  // set the sequence number to `parameter64`
  rewrite_session,   // set the session identifier to `parameter`

  count_
};

inline constexpr auto kFaultOpCount = static_cast<std::size_t>(fault_op::count_);

[[nodiscard]] constexpr std::string_view to_string(fault_op op) noexcept {
  switch (op) {
    case fault_op::none:                    return "none";
    case fault_op::drop:                    return "drop";
    case fault_op::duplicate:               return "duplicate";
    case fault_op::delay:                   return "delay";
    case fault_op::flip_bit:                return "flip_bit";
    case fault_op::truncate:                return "truncate";
    case fault_op::overstate_block_count:   return "overstate_block_count";
    case fault_op::understate_block_count:  return "understate_block_count";
    case fault_op::overstate_block_length:  return "overstate_block_length";
    case fault_op::rewrite_sequence:        return "rewrite_sequence";
    case fault_op::rewrite_session:         return "rewrite_session";
    case fault_op::count_:                  return "count_";
  }
  return "<unknown fault_op>";
}

// Whether the operation changes the bytes rather than only the delivery.
//
// The distinction matters to the injector: a corrupting fault needs a writable
// copy of the packet, a delivery fault does not. Getting it wrong means either
// copying every packet or writing through a const view.
[[nodiscard]] constexpr bool mutates_bytes(fault_op op) noexcept {
  switch (op) {
    case fault_op::flip_bit:
    case fault_op::truncate:
    case fault_op::overstate_block_count:
    case fault_op::understate_block_count:
    case fault_op::overstate_block_length:
    case fault_op::rewrite_sequence:
    case fault_op::rewrite_session:
      return true;

    case fault_op::none:
    case fault_op::drop:
    case fault_op::duplicate:
    case fault_op::delay:
      return false;

    case fault_op::count_:
      break;
  }
  // Every enumerator is listed rather than using a default, so adding one without
  // classifying it is a -Wswitch warning rather than a silent "does not mutate"
  //, which would make the injector hand a const view to a mutating operation.
  return false;
}

// Whether the operation should make a correct receiver report a fatal condition.
//
// This is the injector's half of the oracle. A fault whose expected consequence
// is known lets a test assert that the receiver detected *exactly* the injected
// faults: no more, and no fewer.
[[nodiscard]] constexpr bool expects_fatal_report(fault_op op) noexcept {
  return op == fault_op::rewrite_session;
}

// One scheduled fault.
//
// Trivially copyable, so a schedule is a flat array that can be compared between
// two runs and written to a file without serialisation.
struct fault {
  fault_op op{fault_op::none};

  // The first packet index this applies to, counting from zero over the whole
  // stream. Absolute rather than relative, so deleting an earlier fault during
  // shrinking does not move this one.
  std::uint64_t first_packet{0};

  // How many consecutive packets it covers. One for a point fault.
  //
  // A span rather than an independent probability per packet, because
  // BUILD-GUIDE.md section 5 is emphatic about it: model bursts, not
  // probabilities. A drop_probability of 0.001 tests almost nothing real:
  // consecutive loss is what actually happens and what actually breaks a
  // receiver's recovery window.
  std::uint32_t packet_count{1};

  // Operation-specific. A byte offset for flip_bit, a length for truncate, a
  // displacement for delay, a session id for rewrite_session.
  std::uint32_t parameter{0};

  // Second operand where one is needed: the bit index for flip_bit, or the
  // amount to add or subtract for the count and length rewrites.
  std::uint32_t detail{0};

  // A 64-bit operand, for rewrite_sequence.
  std::uint64_t parameter64{0};

  [[nodiscard]] constexpr bool covers(std::uint64_t packet_index) const noexcept {
    return packet_index >= first_packet &&
           packet_index - first_packet < packet_count;
  }

  [[nodiscard]] constexpr std::uint64_t last_packet() const noexcept {
    return first_packet + packet_count - 1;
  }

  [[nodiscard]] friend constexpr bool operator==(const fault&,
                                                 const fault&) = default;
};

static_assert(std::is_trivially_copyable_v<fault>);

// 40 rather than 32 because `op` is declared first, which costs seven bytes of
// leading padding. Moving it last would pack to 32, and is not worth it: `op`
// first is the order a reader wants, and designated initialisers must follow
// declaration order, so the readable spelling `{.op = ..., .first_packet = ...}`
// depends on it.
//
// The property that actually matters is that a fault fits comfortably inside a
// cache line, so walking a schedule touches few lines. Pinned exactly as well, so
// accidental growth is a compile error rather than something noticed later.
static_assert(sizeof(fault) == 40);
static_assert(sizeof(fault) <= kCacheLineSize,
              "a fault must fit in a cache line so that walking a schedule "
              "touches one line per few entries");

}  // namespace dfr::inline v1::chaos
#endif  // DFR_CHAOS_FAULT_HPP

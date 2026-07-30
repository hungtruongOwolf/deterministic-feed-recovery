// Applying a fault schedule to a stream of packets.
//
// The caller offers each source packet with its index; the injector emits zero,
// one or two packets in response, plus any previously delayed packet whose
// release point has arrived. A callback rather than a returned container, because
// the number of outputs varies and a container would mean allocating per packet.
//
// LIFETIME CONTRACT: an emitted view is valid only until the next call to offer()
// or flush(). A mutated packet lives in the injector's scratch buffer, which the
// next call overwrites. A caller that needs to keep a packet must copy it. This is
// the same contract Aeron's flyweights have and for the same reason — it is what
// makes the zero-copy path possible for the packets that were not damaged at all.
//
// Nothing is allocated after construction. The delay queue and the scratch buffer
// are fixed-size members, per TIGER_STYLE's rule that no memory may be
// dynamically allocated after initialisation.

#ifndef DFR_CHAOS_INJECTOR_HPP
#define DFR_CHAOS_INJECTOR_HPP

#include <dfr/chaos/schedule.hpp>
#include <dfr/chaos/target.hpp>
#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dfr::inline v1 {
namespace chaos {

// Large enough for any non-jumbo Ethernet payload. A source packet above this is
// refused rather than silently truncated, because a truncation the caller did not
// ask for would be attributed to the fault schedule.
inline constexpr std::size_t kMaxPacketBytes = 2048;

// How many packets may be in flight as delayed. Bounded, so the queue is a
// fixed-size member; a schedule asking for more is refused at injection time.
inline constexpr std::size_t kMaxDelayedPackets = 32;

// Why a packet came out the way it did, so a caller can build the oracle: it
// knows what was injected and can assert the receiver reported exactly that.
struct emission {
  packet_view packet;
  fault_op cause{fault_op::none};

  // The source index this packet came from, which is not the current index when
  // the packet was delayed.
  std::uint64_t source_index{0};

  // True for the second copy of a duplicated packet.
  bool is_duplicate{false};
};

// A tally of what actually happened, which is the other half of the oracle. A
// schedule says what was asked for; this says what the stream really did, and the
// two can differ when a packet was too short to damage.
struct injection_stats {
  std::uint64_t offered{0};
  std::uint64_t emitted{0};
  std::uint64_t dropped{0};
  std::uint64_t duplicated{0};
  std::uint64_t delayed{0};
  std::uint64_t mutated{0};

  // A fault the schedule asked for that the packet could not carry — too short
  // for the field, or a delay queue already full. Counted rather than ignored,
  // because a test asserting "the receiver saw exactly the injected faults" must
  // know the difference between a fault that was applied and one that was not.
  std::uint64_t not_applicable{0};

  std::array<std::uint64_t, kFaultOpCount> by_op{};

  [[nodiscard]] friend bool operator==(const injection_stats&,
                                       const injection_stats&) = default;
};

template <fault_target Target>
class injector {
 public:
  constexpr injector() noexcept = default;
  explicit constexpr injector(schedule plan) noexcept : plan_(plan) {}

  [[nodiscard]] constexpr const schedule& plan() const noexcept { return plan_; }
  [[nodiscard]] constexpr const injection_stats& stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] constexpr std::size_t delayed_in_flight() const noexcept {
    return delayed_count_;
  }

  // Offers one source packet. `emit` is called once per output packet.
  //
  // Delayed packets are released *before* the current one is considered, so a
  // packet delayed to index i arrives ahead of the packet natively at i. That is
  // the ordering a real reorder produces: the late packet overtakes nothing, it
  // merely arrives out of position.
  template <typename Emit>
  [[nodiscard]] result<void> offer(packet_view source, std::uint64_t index,
                                   Emit&& emit) noexcept {
    ++stats_.offered;

    if (const auto err = release_due(index, emit); !err) DFR_UNLIKELY {
      return err;
    }

    const fault* entry = plan_.at_packet(index);
    if (entry == nullptr) {
      deliver(emit, emission{.packet = source, .source_index = index});
      return ok();
    }

    return apply(*entry, source, index, emit);
  }

  // Releases everything still delayed, in the order it was queued.
  //
  // Called at the end of a stream. Without it, a packet delayed past the last
  // index would simply vanish — which is a drop, not a reorder, and would make
  // the oracle expect the wrong thing.
  template <typename Emit>
  [[nodiscard]] result<void> flush(Emit&& emit) noexcept {
    for (std::size_t i = 0; i < delayed_count_; ++i) {
      const slot& held = delayed_[i];
      deliver(emit, emission{.packet = view_of(held),
                             .cause = fault_op::delay,
                             .source_index = held.source_index});
    }
    delayed_count_ = 0;
    return ok();
  }

 private:
  struct slot {
    std::array<std::byte, kMaxPacketBytes> bytes{};
    std::size_t size{0};
    std::uint64_t source_index{0};
    std::uint64_t release_index{0};
  };

  template <typename Emit>
  constexpr void deliver(Emit& emit, const emission& out) noexcept {
    ++stats_.emitted;
    emit(out);
  }

  [[nodiscard]] static constexpr packet_view view_of(const slot& s) noexcept {
    return packet_view{s.bytes.data(), s.size};
  }

  template <typename Emit>
  [[nodiscard]] result<void> release_due(std::uint64_t index,
                                         Emit& emit) noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < delayed_count_; ++i) {
      if (delayed_[i].release_index <= index) {
        deliver(emit, emission{.packet = view_of(delayed_[i]),
                               .cause = fault_op::delay,
                               .source_index = delayed_[i].source_index});
      } else {
        // Compacting in place preserves queue order, which matters: two packets
        // delayed to the same index must come out in the order they were sent.
        if (kept != i) {
          delayed_[kept] = delayed_[i];
        }
        ++kept;
      }
    }
    delayed_count_ = kept;
    return ok();
  }

  template <typename Emit>
  [[nodiscard]] result<void> apply(const fault& entry, packet_view source,
                                   std::uint64_t index, Emit& emit) noexcept {
    ++stats_.by_op[static_cast<std::size_t>(entry.op)];

    switch (entry.op) {
      case fault_op::drop:
        ++stats_.dropped;
        return ok();  // emit nothing

      case fault_op::duplicate:
        ++stats_.duplicated;
        deliver(emit, emission{.packet = source, .source_index = index});
        deliver(emit, emission{.packet = source,
                               .cause = fault_op::duplicate,
                               .source_index = index,
                               .is_duplicate = true});
        return ok();

      case fault_op::delay:
        return hold(entry, source, index, emit);

      case fault_op::flip_bit:
      case fault_op::truncate:
      case fault_op::overstate_block_count:
      case fault_op::understate_block_count:
      case fault_op::overstate_block_length:
      case fault_op::rewrite_sequence:
      case fault_op::rewrite_session:
        return damage(entry, source, index, emit);

      case fault_op::none:
      case fault_op::count_:
        DFR_UNREACHABLE("a scheduled fault must name an operation");
    }
    return ok();
  }

  template <typename Emit>
  [[nodiscard]] result<void> hold(const fault& entry, packet_view source,
                                  std::uint64_t index, Emit& emit) noexcept {
    if (delayed_count_ >= kMaxDelayedPackets ||
        source.size() > kMaxPacketBytes) DFR_UNLIKELY {
      // Cannot hold it. Delivering it on time rather than dropping it, and
      // counting the fault as not applied — a silent drop here would be a
      // different fault from the one the schedule named.
      ++stats_.not_applicable;
      deliver(emit, emission{.packet = source, .source_index = index});
      return ok();
    }

    slot& into = delayed_[delayed_count_];
    std::memcpy(into.bytes.data(), source.data(), source.size());
    into.size = source.size();
    into.source_index = index;
    into.release_index = index + entry.parameter;
    ++delayed_count_;
    ++stats_.delayed;
    return ok();
  }

  template <typename Emit>
  [[nodiscard]] result<void> damage(const fault& entry, packet_view source,
                                    std::uint64_t index, Emit& emit) noexcept {
    if (source.size() > kMaxPacketBytes) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    std::memcpy(scratch_.data(), source.data(), source.size());
    std::size_t size = source.size();
    const mutable_packet_view writable{scratch_.data(), size};

    result<void> outcome = ok();
    switch (entry.op) {
      case fault_op::flip_bit:
        if (writable.contains(entry.parameter, 1) && entry.detail < 8) {
          writable.flip_bit_at(entry.parameter, entry.detail);
        } else {
          outcome = error::invalid_argument;
        }
        break;

      case fault_op::truncate:
        // Only shortens. A "truncation" that lengthened the packet would be
        // reading uninitialised scratch, which is a determinism leak as well as a
        // wrong fault.
        if (entry.parameter < size) {
          size = entry.parameter;
        } else {
          outcome = error::invalid_argument;
        }
        break;

      case fault_op::overstate_block_count:
        outcome = Target::adjust_block_count(
            writable, static_cast<int>(entry.detail));
        break;
      case fault_op::understate_block_count:
        outcome = Target::adjust_block_count(
            writable, -static_cast<int>(entry.detail));
        break;
      case fault_op::overstate_block_length:
        outcome = Target::adjust_first_block_length(
            writable, static_cast<int>(entry.detail));
        break;
      case fault_op::rewrite_sequence:
        outcome = Target::add_to_sequence(writable, entry.parameter64);
        break;
      case fault_op::rewrite_session:
        outcome = Target::set_session(writable, entry.parameter);
        break;

      case fault_op::none:
      case fault_op::drop:
      case fault_op::duplicate:
      case fault_op::delay:
      case fault_op::count_:
        DFR_UNREACHABLE("damage() reached a non-mutating operation");
    }

    if (!outcome) {
      // The packet could not carry the fault: too short for the field, or an
      // out-of-range operand. Deliver it untouched and count the fault as not
      // applied, so an oracle does not expect a consequence that was never
      // caused. Silently dropping it instead would be a different fault.
      ++stats_.not_applicable;
      deliver(emit, emission{.packet = source, .source_index = index});
      return ok();
    }

    ++stats_.mutated;
    deliver(emit, emission{.packet = packet_view{scratch_.data(), size},
                           .cause = entry.op,
                           .source_index = index});
    return ok();
  }

  schedule plan_;
  injection_stats stats_{};

  // One scratch buffer, overwritten per mutated packet. See the lifetime contract
  // at the top of this file.
  std::array<std::byte, kMaxPacketBytes> scratch_{};

  std::array<slot, kMaxDelayedPackets> delayed_{};
  std::size_t delayed_count_{0};
};

}  // namespace chaos
}  // namespace dfr::inline v1

#endif  // DFR_CHAOS_INJECTOR_HPP

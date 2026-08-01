// A fault schedule: what happens to which packets, decided once from a seed.
//
// The schedule is generated up front rather than decided per packet, and that is
// the design decision the whole component rests on. Four properties follow from
// it, and none of them is available to a per-packet coin flip:
//
//   * **It is the reproduction artifact.** A failing run is a seed plus a build
//     fingerprint, and the schedule it produced can be printed, diffed and
//     committed. "Reproducible from seed 4711" is good; "here is the three-fault
//     schedule that breaks it" is much better.
//
//   * **It shrinks by deletion.** Removing one fault from the array changes
//     nothing about the others, because every fault names an absolute packet
//     index. A per-packet decision stream does not have that property: disabling
//     one draw shifts every draw after it, and the reduced case stops
//     reproducing. This is why prng::chance(kNever) consumes nothing.
//
//   * **It shrinks open.** The enable mask is stored as *disabled* bits, so the
//     all-zero mask means every fault kind is permitted. Shrinking therefore
//     moves toward more kinds enabled, and the minimal counterexample is stated
//     in the most permissive configuration rather than in a narrow one that looks
//     contrived.
//
//   * **It can be written by hand.** A regression test for a specific defect
//     names the fault it needs instead of hunting for a seed that produces it.

#ifndef DFR_CHAOS_SCHEDULE_HPP
#define DFR_CHAOS_SCHEDULE_HPP

#include <dfr/chaos/fault.hpp>
#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/ratio.hpp>
#include <dfr/core/result.hpp>
#include <dfr/core/rng.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dfr::inline v1 {
namespace chaos {

// A fixed ceiling, so a schedule needs no allocation and can live in a
// simulation's static state. TIGER_STYLE: put a limit on everything, and allocate
// nothing after initialisation.
inline constexpr std::size_t kMaxFaults = 256;

// Which fault kinds a run may use, as *disabled* bits.
//
// Zero means everything is permitted, which is what makes shrinking move toward
// a more permissive configuration rather than a narrower one. This is swarm
// testing at the granularity of a fault kind, the same idea as FoundationDB's
// BUGGIFY activating a random subset of sites per run: each run stays survivable
// while the ensemble covers the cross product.
class op_mask {
 public:
  constexpr op_mask() noexcept = default;

  [[nodiscard]] constexpr bool permits(fault_op op) const noexcept {
    return (disabled_ & bit(op)) == 0;
  }
  constexpr op_mask& disable(fault_op op) noexcept {
    disabled_ |= bit(op);
    return *this;
  }
  constexpr op_mask& enable(fault_op op) noexcept {
    disabled_ &= ~bit(op);
    return *this;
  }

  // Only the named kinds are permitted. Spelled as a factory rather than a
  // constructor so the common case(everything on) stays the default.
  [[nodiscard]] static constexpr op_mask only(
      std::initializer_list<fault_op> ops) noexcept {
    op_mask out;
    out.disabled_ = ~std::uint32_t{0};
    for (const fault_op op : ops) {
      out.enable(op);
    }
    return out;
  }

  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return disabled_; }

  [[nodiscard]] friend constexpr bool operator==(op_mask, op_mask) = default;

 private:
  [[nodiscard]] static constexpr std::uint32_t bit(fault_op op) noexcept {
    return std::uint32_t{1} << static_cast<std::uint32_t>(op);
  }

  std::uint32_t disabled_{0};
};

static_assert(kFaultOpCount <= 32,
              "op_mask holds one bit per fault kind in a uint32");

struct schedule_options {
  // How many faults to place. The generator draws in [min, max].
  std::uint32_t min_faults{1};
  std::uint32_t max_faults{8};

  // Burst length, in consecutive packets.
  //
  // The default upper bound is well above one deliberately. BUILD-GUIDE.md
  // section 5: consecutive loss is what actually happens on a real feed, and a
  // single dropped packet exercises only the easiest branch of a receiver's
  // recovery.
  std::uint32_t min_burst{1};
  std::uint32_t max_burst{10};

  op_mask permitted{};

  // Leave the first N packets alone.
  //
  // A receiver has to establish its position before a gap means anything. Without
  // this, a fault on packet zero is reported as the receiver joining
  // mid-stream: a true statement that tells you nothing about recovery.
  std::uint64_t warmup_packets{4};
};

class schedule {
 public:
  constexpr schedule() noexcept = default;

  // Places faults over a stream of `stream_length` packets.
  //
  // Every draw is bounded and the loop is bounded, so generation always
  // terminates and consumes a number of draws that depends only on the seed and
  // the options: never on the stream's contents.
  [[nodiscard]] static result<schedule> generate(
      prng& rng, const schedule_options& options,
      std::uint64_t stream_length) noexcept {
    if (options.min_faults > options.max_faults) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    if (options.min_burst == 0 || options.min_burst > options.max_burst)
        DFR_UNLIKELY {
      return error::invalid_argument;
    }
    if (options.max_faults > kMaxFaults) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    // Collect the permitted kinds once. Drawing a kind and rejecting it would
    // make the number of draws depend on the mask, so disabling one kind would
    // shift every later draw: the exact property this design exists to avoid.
    std::array<fault_op, kFaultOpCount> allowed{};
    std::size_t allowed_count = 0;
    for (std::size_t i = 1; i < kFaultOpCount; ++i) {  // skip fault_op::none
      const auto op = static_cast<fault_op>(i);
      if (options.permitted.permits(op)) {
        allowed[allowed_count] = op;
        ++allowed_count;
      }
    }
    if (allowed_count == 0) DFR_UNLIKELY {
      return error::invalid_argument;
    }

    // Nothing to schedule onto. Not an error: a caller sweeping seeds against a
    // short capture should get an empty schedule rather than a failure.
    //
    // The threshold accounts for min_burst, not just the warmup, because
    // min_burst is honoured rather than clamped away: see the placement bound
    // below.
    schedule out;
    const std::uint64_t needed =
        options.warmup_packets + options.min_burst;
    if (stream_length < needed) {
      return out;
    }
    const std::uint64_t first = options.warmup_packets;
    const std::uint64_t last = stream_length - 1;

    // The last index at which a full min_burst still fits.
    //
    // Constraining placement rather than clamping the burst afterwards. Clamping
    // was the first implementation and a test caught it: a fault landing near the
    // end came out one packet long even with min_burst set to three, so a caller
    // asking for multi-packet loss silently got a single-packet fault and a test
    // written to exercise burst recovery quietly stopped doing so. Honouring the
    // floor is the contract a caller reads into min_burst.
    const std::uint64_t latest_start = last - (options.min_burst - 1);

    const std::uint64_t count =
        rng.between(options.min_faults, options.max_faults);
    for (std::uint64_t i = 0; i < count; ++i) {
      fault entry;
      entry.op = allowed[rng.index(allowed_count)];
      entry.first_packet = rng.between(first, latest_start);

      const std::uint64_t burst =
          rng.between(options.min_burst, options.max_burst);
      // Still clamped, for the case where max_burst reaches past the end. The
      // clamp can no longer take the length below min_burst, because placement
      // guaranteed room for it. Clamping rather than redrawing keeps the draw
      // count independent of the stream length.
      const std::uint64_t room = last - entry.first_packet + 1;
      entry.packet_count =
          static_cast<std::uint32_t>(burst < room ? burst : room);
      DFR_ASSERT(entry.packet_count >= options.min_burst,
                 "placement must leave room for the minimum burst");

      fill_parameters(rng, entry);
      // Cannot fail: `count` is bounded by max_faults, which was checked against
      // kMaxFaults above. Asserted rather than ignored so the reasoning is
      // recorded and a future change to either bound trips here.
      const auto added = out.add(entry);
      DFR_ASSERT(added.has_value(),
                 "schedule capacity was validated before generation");
    }
    return out;
  }

  // Appends one fault. Used by generate() and by a hand-written regression test,
  // which is the second reason a schedule is data.
  constexpr result<void> add(const fault& entry) noexcept {
    if (count_ >= kMaxFaults) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }
    DFR_ASSERT(entry.op != fault_op::none, "a scheduled fault must do something");
    DFR_ASSERT(entry.packet_count > 0, "a fault must cover at least one packet");
    faults_[count_] = entry;
    ++count_;
    return ok();
  }

  // Removes the fault at `index`, preserving the order of the rest.
  //
  // The shrinker's only structural operation. Order-preserving because a reader
  // comparing two schedules should see one entry missing rather than a
  // permutation.
  constexpr result<void> remove(std::size_t index) noexcept {
    if (index >= count_) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    for (std::size_t i = index; i + 1 < count_; ++i) {
      faults_[i] = faults_[i + 1];
    }
    --count_;
    return ok();
  }

  // DFR_LIFETIME_BOUND is load-bearing here, not decoration. The span points into
  // this object, so `for (auto& f : make_schedule().faults())` iterates a
  // destroyed temporary. That mistake appeared in this file's own tests, passed at
  // -O0 because the stack had not been reused yet, and was caught by
  // AddressSanitizer as a stack-use-after-scope. The annotation turns it into a
  // compiler diagnostic instead.
  [[nodiscard]] constexpr std::span<const fault> faults()
      const noexcept DFR_LIFETIME_BOUND {
    return {faults_.data(), count_};
  }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

  // The first fault covering `packet_index`, or nullptr.
  //
  // First rather than all: two faults on one packet would compose in an order
  // that depends on the array, which is a confusing thing to reason about in a
  // counterexample. The injector applies one fault per packet and the schedule
  // says which.
  [[nodiscard]] constexpr const fault* at_packet(
      std::uint64_t packet_index) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (faults_[i].covers(packet_index)) {
        return &faults_[i];
      }
    }
    return nullptr;
  }

  // The highest packet index any fault touches. Zero for an empty schedule.
  [[nodiscard]] constexpr std::uint64_t last_affected_packet() const noexcept {
    std::uint64_t highest = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const std::uint64_t last = faults_[i].last_packet();
      if (last > highest) {
        highest = last;
      }
    }
    return highest;
  }

  [[nodiscard]] friend constexpr bool operator==(const schedule& lhs,
                                                 const schedule& rhs) noexcept {
    if (lhs.count_ != rhs.count_) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.count_; ++i) {
      if (!(lhs.faults_[i] == rhs.faults_[i])) {
        return false;
      }
    }
    return true;
  }

 private:
  // Operand ranges are chosen to stay plausible: a byte offset inside a small
  // header, a bit index inside a byte, a displacement a receiver could actually
  // buffer. A corruption at byte 4,000,000,000 is refused by the injector rather
  // than testing anything.
  static void fill_parameters(prng& rng, fault& entry) noexcept {
    switch (entry.op) {
      case fault_op::flip_bit:
        entry.parameter = static_cast<std::uint32_t>(rng.between(0, 63));
        entry.detail = static_cast<std::uint32_t>(rng.between(0, 7));
        break;
      case fault_op::truncate:
        entry.parameter = static_cast<std::uint32_t>(rng.between(0, 63));
        break;
      case fault_op::delay:
        entry.parameter = static_cast<std::uint32_t>(rng.between(1, 16));
        break;
      case fault_op::overstate_block_count:
      case fault_op::understate_block_count:
      case fault_op::overstate_block_length:
        entry.detail = static_cast<std::uint32_t>(rng.between(1, 8));
        break;
      case fault_op::rewrite_sequence:
        // A displacement rather than an absolute value, so the rewrite means
        // "jump forward by this much" independently of where the stream is.
        entry.parameter64 = rng.between(1, 1'000);
        break;
      case fault_op::rewrite_session:
        entry.parameter = static_cast<std::uint32_t>(rng.between(1, 0xFFFF'FFFF));
        break;

      case fault_op::drop:
      case fault_op::duplicate:
        break;  // no operands

      case fault_op::none:
      case fault_op::count_:
        DFR_UNREACHABLE("fill_parameters reached a non-operation");
    }
  }

  std::array<fault, kMaxFaults> faults_{};
  std::size_t count_{0};
};

}  // namespace chaos
}  // namespace dfr::inline v1

#endif  // DFR_CHAOS_SCHEDULE_HPP

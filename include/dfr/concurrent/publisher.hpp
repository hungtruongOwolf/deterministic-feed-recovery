// The thread boundary, wired into the pipeline rather than sitting beside it.
//
// `spsc_ring` existed, was benchmarked, was tested under ThreadSanitizer, and **nothing in the architecture used
// it.** A reviewer's first question about a lock-free structure is "is this wired to anything?", and the answer was
// no: it was a component on a shelf. That is a worse state than not having one, because it looks like a claim.
//
// This is the seam made load-bearing. A recovery client delivers messages on the thread that owns the protocol
// state; this takes each one and offers it to a consumer on another core. It is fifty lines, and its whole value is
// that the two halves of the system now actually meet.
//
// Sequence order, not arrival order — enforced here so no caller has to remember
// -----------------------------------------------------------------------------
// The hardest defect this project found is that a consumer of a gap-filling feed must apply messages in **sequence
// order**. While a hole is open the client keeps delivering later messages, deliberately, because stalling on a gap
// turns one loss into an outage — so a repair arrives after higher sequence numbers, and an aggregated book is
// last-write-wins. Applying the older update second leaves the wrong size at that price permanently.
//
// A consumer thread cannot reorder what it is handed without buffering, and asking every consumer to buffer is
// asking every consumer to rediscover this. So the reordering happens **before** the ring: what crosses the
// boundary is already in order, and the far side can be a loop with no memory. That is the right side of the seam
// for it, because the producer already knows the sequence numbers and the consumer would have to be told.
//
// The cost is that a repair holds up the messages behind it — which is correct rather than unfortunate. Handing a
// consumer a book it must not act on yet would be worse than handing it nothing.

#ifndef DFR_CONCURRENT_PUBLISHER_HPP
#define DFR_CONCURRENT_PUBLISHER_HPP

#include <dfr/concurrent/delivery.hpp>
#include <dfr/concurrent/spsc_ring.hpp>
#include <dfr/core/assert.hpp>
#include <dfr/core/packet_view.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace concurrent {

struct publisher_stats {
  std::uint64_t offered{0};
  std::uint64_t published{0};
  /** Held back because a lower sequence had not arrived. Transient; a non-zero value here is normal. */
  std::uint64_t reordered{0};
  /** Refused by a full ring. The consumer is behind, and this is the number that says by how much. */
  std::uint64_t refused{0};
  /** Too large for a delivery record, which is a configuration error rather than a busy moment. */
  std::uint64_t oversized{0};

  [[nodiscard]] friend constexpr bool operator==(const publisher_stats&,
                                                 const publisher_stats&) = default;
};

/**
 * Puts delivered messages across a thread boundary, in sequence order.
 *
 * `Pending` bounds how far out of order the producer will hold. A gap wider than this cannot be bridged and the
 * messages above it are refused rather than reordered — bounded like everything else here, because an unbounded
 * buffer turns a slow retransmit into memory growth nobody chose.
 */
template <std::size_t Capacity = 4096, std::size_t Pending = 256>
class publisher {
 public:
  using ring_type = spsc_ring<delivery, Capacity>;

  explicit constexpr publisher(std::uint64_t first_sequence) noexcept
      : next_(first_sequence) {}

  [[nodiscard]] constexpr const publisher_stats& stats() const noexcept { return stats_; }
  [[nodiscard]] ring_type& ring() noexcept DFR_LIFETIME_BOUND { return ring_; }
  /** The sequence the consumer is still waiting for. Diagnostic: the producer's own view. */
  [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept { return next_; }

  /**
   * Offers one delivered message.
   *
   * Returns false when the ring refused it or the body does not fit — both of which the caller must see, because
   * both mean the consumer's book is about to be incomplete and only the caller can decide what to do about it.
   */
  [[nodiscard]] bool offer(std::uint64_t sequence, std::uint8_t line, bool recovered,
                           packet_view body) noexcept {
    ++stats_.offered;

    if (sequence < next_) {
      // Already published. A duplicate the client let through, or a repair for something the buffer bridged; either
      // way, publishing it again would apply an older update over a newer one.
      return true;
    }

    delivery record;
    if (!delivery::from(sequence, line, recovered, body, record)) DFR_UNLIKELY {
      ++stats_.oversized;
      return false;
    }

    if (sequence == next_) {
      if (!publish(record)) {
        return false;
      }
      ++next_;
      return drain_pending();
    }

    // Out of order: hold it until the sequence below arrives.
    const auto slot = sequence - next_;
    if (slot > Pending) DFR_UNLIKELY {
      ++stats_.refused;
      return false;
    }
    ++stats_.reordered;
    held_[(sequence) % Pending] = record;
    have_[(sequence) % Pending] = true;
    return true;
  }

 private:
  [[nodiscard]] bool publish(const delivery& record) noexcept {
    if (!ring_.push(record)) DFR_UNLIKELY {
      ++stats_.refused;
      return false;
    }
    ++stats_.published;
    return true;
  }

  // Everything now contiguous above `next_`.
  [[nodiscard]] bool drain_pending() noexcept {
    while (have_[next_ % Pending]) {
      const auto slot = next_ % Pending;
      // The slot is indexed modulo `Pending`, so a sequence exactly `Pending` ahead would alias one already held.
      // The bound in offer() is what stops that, and this asserts the two agree rather than trusting they do.
      DFR_ASSERT_PARANOID(held_[slot].sequence == next_,
                          "a held delivery aliased another sequence in the pending window");
      if (!publish(held_[slot])) {
        return false;
      }
      have_[slot] = false;
      ++next_;
    }
    return true;
  }

  ring_type ring_{};
  std::uint64_t next_{1};
  publisher_stats stats_{};
  // A flat window rather than a map: the reorder distance is small and bounded, and a node per held message would
  // allocate on the path that runs while the market is moving.
  std::array<delivery, Pending> held_{};
  std::array<bool, Pending> have_{};
};

}  // namespace concurrent
}  // namespace dfr::inline v1

#endif  // DFR_CONCURRENT_PUBLISHER_HPP

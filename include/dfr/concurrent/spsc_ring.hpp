// A single-producer, single-consumer ring for handing recovered messages to a consumer thread.
//
// Where the threads go, and why not in the core
// ---------------------------------------------
// The recovery core is single-threaded and stays that way. That is not caution: determinism is the property
// the whole project is built on(a failing run must replay from a seed) and a multi-threaded core would
// make the interleaving part of the input. There would be nothing left to reproduce.
//
// But a feed handler that never leaves its own thread is not a feed handler. Somebody downstream has to
// consume what recovery delivers, and in every real system that somebody is on another core. So the
// concurrency lives exactly where production systems put it: one seam, between the thread that owns the
// protocol state and the thread that owns the strategy. The core never learns that a second thread exists.
//
// Full means refused, never overwritten
// -------------------------------------
// When the consumer falls behind, `push` fails and says so. It does not overwrite the oldest record: the
// same decision recovery::replay_buffer and trace::recorder make, for the same reason applied to a third
// place: overwriting turns a *known* backlog into a silent hole. A refused message is still in the caller's
// hands and the caller already knows how to report a gap; an overwritten one is data loss nobody recorded.
//
// A dropping ring is a defensible design for some feeds(stale quotes are worthless) but it must be the
// caller's choice, made where the caller can account for it. `refused()` is how this one accounts.
//
// What makes it fast, stated rather than assumed
// ---------------------------------------------
//   * The two indices sit on separate cache lines, so a producer write does not invalidate the line the
//     consumer reads its own index from. **Measured at 10–25%**, consistently, not the order of magnitude I
//     assumed before measuring it: see docs/CONCURRENCY.md. The floor underneath both is core-to-core cache
//     line transfer, which no arrangement of the code removes.
//   * Each side caches the other's index and only re-reads it when its own cached copy says the ring looks
//     full or empty. In the common case neither thread touches the other's cache line at all.
//   * Capacity is a power of two, checked at compile time, so the wrap is a mask rather than a modulo.
//   * `memory_order_acquire` / `memory_order_release` only. A `seq_cst` fence on a hot path buys nothing
//     here: there is one producer and one consumer, so there is no third party whose view of the ordering
//     could disagree.
//
// The correctness argument is the standard one and worth writing down: the producer publishes a slot's
// contents *before* releasing the new tail, and the consumer acquires the tail *before* reading the slot. The
// release/acquire pair is what makes the slot's bytes visible; without it the index could arrive first and
// the consumer would read a half-written record.

#ifndef DFR_CONCURRENT_SPSC_RING_HPP
#define DFR_CONCURRENT_SPSC_RING_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/narrow.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace dfr::inline v1 {
namespace concurrent {

// The line size to pad to comes from core/attributes.hpp, and this file used to define its own.
//
// Two definitions of the cache line is the bug: fixing the ABI dependency in one of them left the other still
// reading `std::hardware_destructive_interference_size`, whose value is part of the GCC ABI, so two translation
// units built with different GCC versions could disagree about how wide this ring's padded members are, which is
// a layout mismatch rather than a performance question. GCC said so; one definition is the fix.

template <typename T, std::size_t Capacity>
class spsc_ring {
 public:
  static_assert(Capacity >= 2, "a ring needs room for at least two records");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "capacity must be a power of two, so the wrap is a mask rather than a division");
  static_assert(std::is_trivially_copyable_v<T>,
                "a record crossing a thread boundary must be copyable without running code");

  constexpr spsc_ring() noexcept = default;

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // How many records the producer has offered that the ring could not take.
  //
  // Read from either thread. It is the consumer falling behind made countable, which is the whole reason
  // refusing beats overwriting.
  [[nodiscard]] std::uint64_t refused() const noexcept {
    return refused_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t pushed() const noexcept {
    return tail_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t popped() const noexcept {
    return head_.load(std::memory_order_relaxed);
  }

  // An instantaneous count, which is exactly as meaningful as that sounds.
  //
  // Between the two loads either side may have moved, so this is a diagnostic and never a decision. A caller
  // that branched on it would be racing itself; `push` and `pop` report their own success instead.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const auto tail = tail_.load(std::memory_order_acquire);
    const auto head = head_.load(std::memory_order_acquire);
    return narrowed<std::size_t>(tail - head);
  }

  // ---- producer side -----------------------------------------------------

  // Offers one record. Returns false if the ring is full, in which case nothing was written.
  //
  // `bool` rather than `void` because a full ring is a normal condition a caller must handle, and rather than
  // `result<void>` because there is exactly one reason it can fail and naming it in a return type would be
  // ceremony. TIGER_STYLE's ordering(void, then bool, then a richer type) points here.
  [[nodiscard]] bool push(const T& value) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);

    // The cached head is only a lower bound on where the consumer has reached, so a stale copy can make the
    // ring *look* full when it is not. Re-reading is the slow path and this is the branch that keeps it slow:
    // in the common case the cached value already proves there is room and the consumer's cache line is never
    // touched.
    if (tail - cached_head_ >= Capacity) DFR_UNLIKELY {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ >= Capacity) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    }

    slots_[tail & (Capacity - 1)] = value;
    // Release: everything written above, including the slot, is visible to any thread that acquires this.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // ---- consumer side -----------------------------------------------------

  // Takes one record. Returns false if the ring is empty, in which case `into` is untouched.
  [[nodiscard]] bool pop(T& into) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);

    if (head >= cached_tail_) DFR_UNLIKELY {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head >= cached_tail_) {
        return false;
      }
    }

    into = slots_[head & (Capacity - 1)];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Takes up to `limit` records into `out`, and returns how many.
  //
  // The largest win available here, and larger than the padding: one cache-line transfer serves the whole
  // batch instead of one per record. Measured at roughly **2× faster per message** at a batch of 64, against
  // 10–25% for the padding. A caller consuming one at a time is paying for a core-to-core round trip per
  // message, and no arrangement of the producer's code can give that back. See bench/handoff_bench.
  [[nodiscard]] std::size_t pop_batch(T* out, std::size_t limit) noexcept {
    DFR_ASSERT(out != nullptr || limit == 0, "a batch needs somewhere to go");
    const auto head = head_.load(std::memory_order_relaxed);
    cached_tail_ = tail_.load(std::memory_order_acquire);

    const auto available = narrowed<std::size_t>(cached_tail_ - head);
    const std::size_t taking = available < limit ? available : limit;
    for (std::size_t i = 0; i < taking; ++i) {
      out[i] = slots_[(head + i) & (Capacity - 1)];
    }
    if (taking > 0) {
      head_.store(head + taking, std::memory_order_release);
    }
    return taking;
  }

 private:
  // Padded so the producer's writes and the consumer's writes never share a line. Without this, every push
  // invalidates the line the consumer is reading its own index from, and the ring runs an order of magnitude
  // slower for a reason no profiler attributes to the right place.
  alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
  // Producer-private: the consumer's index as the producer last saw it.
  alignas(kCacheLineSize) std::uint64_t cached_head_{0};

  alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};
  // Consumer-private.
  alignas(kCacheLineSize) std::uint64_t cached_tail_{0};

  alignas(kCacheLineSize) std::atomic<std::uint64_t> refused_{0};

  // Records, not pointers. A ring of pointers would move the ownership question somewhere else and add a
  // dependent load on the consumer's critical path.
  std::array<T, Capacity> slots_{};
};

}  // namespace concurrent
}  // namespace dfr::inline v1

#endif  // DFR_CONCURRENT_SPSC_RING_HPP

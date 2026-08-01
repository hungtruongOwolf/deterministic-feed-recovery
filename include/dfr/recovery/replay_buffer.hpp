// Holding live messages while a snapshot is being fetched, so they can be replayed
// on top of it.
//
// Linear, not a ring, and that is the most important decision in the file
// -----------------------------------------------------------------------
// A ring buffer is the obvious choice and the wrong one. When a ring fills it
// overwrites its own oldest entries(silently, by construction) and the oldest
// entries are exactly the ones that sit between the snapshot's position and the live
// feed. Losing them is what makes the Glimpse race undetectable: the client applies the
// snapshot, replays what it still has, and produces a book that is plausible, complete
// looking, and permanently missing a range nobody counted.
//
// So this buffer fills up and refuses. That is not a limitation to apologise for; it is
// the whole reason to write it. A recovery attempt that cannot hold the live feed for as
// long as the snapshot takes has failed, and the caller needs to hear that and start
// again with a bigger buffer or a faster snapshot facility.
//
// Two capacities, because either can run out first
// -----------------------------------------------
// Bytes and message count are bounded independently. A feed of many tiny messages
// exhausts the index while the arena is nearly empty; a feed of large messages does the
// opposite. An implementation that checks only one of them works fine right up until the
// traffic mix changes.
//
// Contiguity is enforced rather than tracked
// ------------------------------------------
// A hole in the replay buffer means the replay produces a wrong book, so a
// non-contiguous append is refused. That is achievable because what gets buffered is the
// *merged* stream(after the arbiter has combined the redundant lines) rather than one
// line's raw arrivals.

#ifndef DFR_RECOVERY_REPLAY_BUFFER_HPP
#define DFR_RECOVERY_REPLAY_BUFFER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/narrow.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/gap.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dfr::inline v1 {
namespace recovery {

// Sized by the caller from the snapshot's expected latency times the feed's message
// rate, with headroom. The defaults are deliberately modest: an object this size is a
// member, not a stack local, and a caller that needs a megabyte should say so rather
// than inherit it.
template <std::size_t Bytes = 64 * 1024, std::size_t Messages = 1024>
class replay_buffer {
 public:
  static_assert(Bytes > 0 && Messages > 0,
                "a replay buffer with no capacity would refuse every append");

  constexpr replay_buffer() noexcept = default;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] constexpr std::size_t bytes_used() const noexcept { return used_; }
  [[nodiscard]] static constexpr std::size_t byte_capacity() noexcept { return Bytes; }
  [[nodiscard]] static constexpr std::size_t message_capacity() noexcept {
    return Messages;
  }

  // The contiguous span of sequence numbers held. Empty when nothing is buffered.
  [[nodiscard]] constexpr sequence_range buffered() const noexcept {
    return count_ == 0 ? sequence_range{}
                       : sequence_range{.first = first_sequence_,
                                        .end = first_sequence_ + count_};
  }

  // Appends the next message in sequence.
  //
  // A message already held is accepted and ignored, because a duplicate arriving during
  // recovery is routine: the second copy from a redundant line, or a retransmit that
  // crossed with the live feed. A message that would leave a hole is refused: the
  // replay has to be contiguous or the book it produces is wrong.
  [[nodiscard]] constexpr result<void> append(std::uint64_t sequence,
                                              packet_view message) noexcept {
    if (count_ == 0) {
      first_sequence_ = sequence;
    } else {
      const std::uint64_t expected = first_sequence_ + count_;
      if (sequence < expected) {
        return ok();  // already held; nothing to do and not an error
      }
      if (sequence > expected) DFR_UNLIKELY {
        return error::sequence_gap;
      }
    }

    if (count_ >= Messages || used_ + message.size() > Bytes) DFR_UNLIKELY {
      // Refused, not overwritten. See the note at the top of this file: a buffer that
      // silently dropped its oldest entries here is what makes a permanently wrong book
      // look like a correct one.
      if (count_ == 0) {
        first_sequence_ = 0;  // never actually held anything
      }
      return error::recovery_buffer_overflow;
    }

    if (message.size() > 0) {
      std::memcpy(arena_.data() + used_, message.data(), message.size());
    }
    index_[count_] = span{.offset = used_, .length = message.size()};
    used_ += message.size();
    ++count_;
    return ok();
  }

  // Discards messages below `sequence`, which is what a snapshot covering them makes
  // correct. Returns how many were dropped.
  //
  // Compacting the arena rather than tracking a moving base: this happens once per
  // recovery attempt, not per message, so the simpler shape is worth more than the
  // saved memmove, and a moving base is how an off-by-one turns into reading someone
  // else's message.
  [[nodiscard]] constexpr std::size_t drop_below(std::uint64_t sequence) noexcept {
    if (count_ == 0 || sequence <= first_sequence_) {
      return 0;
    }
    const std::uint64_t held_end = first_sequence_ + count_;
    if (sequence >= held_end) {
      const std::size_t dropped = count_;
      clear();
      return dropped;
    }

    const std::size_t dropped = narrowed<std::size_t>(sequence - first_sequence_);
    const std::size_t byte_offset = index_[dropped].offset;
    const std::size_t remaining_bytes = used_ - byte_offset;

    std::memmove(arena_.data(), arena_.data() + byte_offset, remaining_bytes);
    for (std::size_t i = dropped; i < count_; ++i) {
      index_[i - dropped] = span{.offset = index_[i].offset - byte_offset,
                                 .length = index_[i].length};
    }
    count_ -= dropped;
    used_ = remaining_bytes;
    first_sequence_ = sequence;
    return dropped;
  }

  // Hands every buffered message to the handler, in sequence order.
  //
  // The view is valid only for the duration of the call, like every other view in this
  // library. A handler that needs to keep a message copies it.
  template <typename Handler>
  constexpr void replay(Handler&& handler) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      handler(first_sequence_ + i,
              packet_view{arena_.data() + index_[i].offset, index_[i].length});
    }
  }

  // One message by sequence number, for a caller replaying at its own pace.
  [[nodiscard]] constexpr result<packet_view> at(
      std::uint64_t sequence) const noexcept DFR_LIFETIME_BOUND {
    if (!buffered().contains(sequence)) {
      return error::invalid_argument;
    }
    const std::size_t i = narrowed<std::size_t>(sequence - first_sequence_);
    return packet_view{arena_.data() + index_[i].offset, index_[i].length};
  }

  constexpr void clear() noexcept {
    count_ = 0;
    used_ = 0;
    first_sequence_ = 0;
  }

 private:
  struct span {
    std::size_t offset{0};
    std::size_t length{0};
  };

  std::array<std::byte, Bytes> arena_{};
  std::array<span, Messages> index_{};
  std::size_t count_{0};
  std::size_t used_{0};
  std::uint64_t first_sequence_{0};
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_REPLAY_BUFFER_HPP

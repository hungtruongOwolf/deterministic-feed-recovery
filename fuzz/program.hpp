// A fuzz input read as a program, not as a packet.
//
// The seven decoder targets fuzz bytes: does it crash, and is the answer self-consistent. That covers the
// layer where hostile input actually arrives, and it does not touch the part of this library most likely to be
// wrong. `recovery::client` is a state machine with four states, a retransmit timer, a reorder buffer and a
// snapshot path that can arrive at any moment, and its defects are *sequences of legal calls* rather than
// malformed bytes. Mutating a packet will never produce "a snapshot lands while two holes are open and the second
// retransmit is in flight".
//
// So the input is decoded as a program. Each step takes a byte for the opcode and a few more for its operands,
// and the operands are folded into legal ranges rather than validated: a fuzzer that spends its budget being
// rejected at the door explores the door. What is being explored here is the *order* of operations.
//
// The venue model is deliberately thin. It is not a second implementation of the protocol; it tracks only what is
// needed to state an invariant the client cannot check for itself: which sequence numbers were ever published, so
// that "the client delivered something nobody sent" is expressible.

#ifndef DFR_FUZZ_PROGRAM_HPP
#define DFR_FUZZ_PROGRAM_HPP

#include <dfr/core/packet_view.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr_fuzz {

// Pulls operands out of the input, and reports when there is nothing left.
//
// Returns zero past the end rather than wrapping. Wrapping would make a short input into an infinite program, and
// then every crash report would carry a length that does not reproduce it.
class program_reader {
 public:
  explicit constexpr program_reader(dfr::packet_view input) noexcept : input_(input) {}

  [[nodiscard]] constexpr bool done() const noexcept { return at_ >= input_.size(); }

  [[nodiscard]] constexpr std::uint8_t byte() noexcept {
    if (done()) {
      return 0;
    }
    return input_.u8_at(at_++);
  }

  /** A byte folded into [0, bound). Folded rather than rejected: every input is a valid program. */
  [[nodiscard]] constexpr std::uint32_t upto(std::uint32_t bound) noexcept {
    if (bound == 0) {
      return 0;
    }
    return static_cast<std::uint32_t>(byte()) % bound;
  }

 private:
  dfr::packet_view input_;
  std::size_t at_{0};
};

// What the venue is known to have sent.
//
// One number and one flag, because that is all an invariant needs. Anything richer would be a second protocol
// implementation, and when the two disagreed the fuzzer would report the model's bug as the library's.
struct venue_model {
  /** The next sequence the venue will assign. Starts at 1 like every real feed here. */
  std::uint64_t next_sequence{1};
  /** The highest sequence ever published on any line, across every session. */
  std::uint64_t highest_published{0};
  std::uint32_t session{0xF0F0};

  /** Publishes `count` messages and returns the range, without saying which lines carried it. */
  constexpr std::uint64_t publish(std::uint64_t count) noexcept {
    const std::uint64_t first = next_sequence;
    next_sequence += count;
    if (next_sequence - 1 > highest_published) {
      highest_published = next_sequence - 1;
    }
    return first;
  }

  /** A session change. Everything the client held becomes meaningless, which is the point of testing it. */
  constexpr void reset_session(std::uint32_t to) noexcept {
    session = to;
    next_sequence = 1;
    // `highest_published` is deliberately not reset: it is the ceiling for "was this ever sent", and a client
    // must not deliver above the highest number the venue has ever used, in any session.
  }
};

// The opcodes.
//
// Ordered by how often they should happen rather than alphabetically, because the fold in `upto()` makes the low
// values no more likely than the high ones, and the weighting is done in the dispatch instead.
enum class op : std::uint8_t {
  publish_in_order,
  publish_after_loss,
  publish_duplicate,
  publish_on_second_line,
  publish_overlapping,
  publish_out_of_session,
  deliver_retransmit,
  refuse_retransmit,
  advance_clock,
  poll,
  deliver_snapshot,
  finish_replay,
  count_,
};

[[nodiscard]] constexpr op op_at(std::uint8_t raw) noexcept {
  return static_cast<op>(raw % static_cast<std::uint8_t>(op::count_));
}

}  // namespace dfr_fuzz

#endif  // DFR_FUZZ_PROGRAM_HPP

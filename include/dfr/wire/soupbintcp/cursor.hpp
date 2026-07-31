// Walking a TCP byte stream into SoupBinTCP packets, and counting the implicit sequence.
//
// Two jobs that belong together because neither is useful alone: framing a stream that arrives in
// arbitrary pieces, and keeping the sequence number the protocol never puts on the wire.
//
// The cursor never copies and never buffers. It reads from a view the caller owns and reports how
// much of it was consumed, so the caller keeps whatever partial packet is left over and appends to
// it. Owning a buffer here would mean choosing its size for every caller, and a recovery client and
// an order-entry session want very different ones.

#ifndef DFR_WIRE_SOUPBINTCP_CURSOR_HPP
#define DFR_WIRE_SOUPBINTCP_CURSOR_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/soupbintcp/packet.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace wire::soupbintcp {

// A packet together with the sequence number it turned out to have.
//
// The number is attached here rather than in `packet`, because a packet decoded on its own has no
// sequence: the value comes from counting, and only a cursor counts.
struct sequenced_packet {
  packet frame{};
  // The sequence of this packet, for a Sequenced Data Packet. Zero for every other type, since they
  // have none — not "unknown", none.
  std::uint64_t sequence{0};
};

class stream_cursor {
 public:
  constexpr stream_cursor() noexcept = default;

  // Starts counting from the sequence Login Accepted named: the number of the *next* Sequenced Data
  // Packet, not the last one sent.
  explicit constexpr stream_cursor(std::uint64_t next_sequence) noexcept
      : next_sequence_(next_sequence), started_(true) {}

  [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept {
    return next_sequence_;
  }
  [[nodiscard]] constexpr bool started() const noexcept { return started_; }
  [[nodiscard]] constexpr std::size_t consumed() const noexcept { return consumed_; }
  [[nodiscard]] constexpr std::uint64_t packets() const noexcept { return packets_; }
  [[nodiscard]] constexpr std::uint64_t sequenced_packets() const noexcept {
    return sequenced_;
  }

  // Adopts the position from a Login Accepted packet. Separate from the constructor because a
  // session is often built before it has logged in, and a cursor that had to be reconstructed at
  // that moment would lose its counts.
  constexpr void accept_login(std::uint64_t next_sequence) noexcept {
    next_sequence_ = next_sequence;
    started_ = true;
  }

  // Reads the next packet out of `stream`, which must be the bytes not yet consumed.
  //
  // On `need_more_bytes` nothing is consumed and the same view can be passed again once more bytes
  // are appended. The caller keeps the buffer; this only says how far into it the packets reach.
  [[nodiscard]] constexpr result<sequenced_packet> next(packet_view stream) noexcept {
    packet frame;
    if (const auto err = decode(stream).get(frame); err != error::ok) {
      return err;
    }

    consumed_ += frame.frame_size;
    ++packets_;

    std::uint64_t sequence = 0;
    if (frame.advances_sequence()) {
      // A client that has not logged in cannot number what it is receiving, and guessing would give
      // every message a position that looks authoritative and is invented.
      if (!started_) DFR_UNLIKELY {
        return error::sequence_reset;
      }
      sequence = next_sequence_;
      ++next_sequence_;
      ++sequenced_;
    }

    return sequenced_packet{.frame = frame, .sequence = sequence};
  }

  // Forgets the position without forgetting the counts, for a session that has ended.
  //
  // The counts survive because they describe the connection, and a caller reporting how much a
  // session carried before it dropped needs them after it has dropped.
  constexpr void end_session() noexcept {
    started_ = false;
    next_sequence_ = 0;
  }

 private:
  std::uint64_t next_sequence_{0};
  std::size_t consumed_{0};
  std::uint64_t packets_{0};
  std::uint64_t sequenced_{0};
  bool started_{false};
};

}  // namespace wire::soupbintcp
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_SOUPBINTCP_CURSOR_HPP

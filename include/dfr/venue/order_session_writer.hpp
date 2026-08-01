// How an order-entry session writes: framing, and the numbering that goes with it.
//
// Split from order_session.hpp at the seam the file was over its length limit across, and it is a real seam rather
// than a line count: a reader checking the login rules or the phase transitions never needs to know how a frame is
// built, and the frame builder has no opinion about phases.
//
// What lives here is the half that owns the **outbound sequence number**, which is the session's one piece of
// bookkeeping nobody else can do. SoupBinTCP puts that number nowhere in the packet, so a client derives it by
// counting, and the whole of `order_session_test.cpp` turns on the two arriving at the same answer.

#ifndef DFR_VENUE_ORDER_SESSION_WRITER_HPP
#define DFR_VENUE_ORDER_SESSION_WRITER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/venue/order_session_state.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::venue {

// Builds the frames a session sends, and counts what it numbered.
//
// Holds the sequence rather than taking it as a parameter: an outbound stream has exactly one counter, and passing
// it in would let two callers disagree about it, which is the defect this whole component exists to make
// impossible.
class order_session_writer {
 public:
  // Every acknowledgement is one OUCH message inside one Sequenced Data Packet, so the largest frame is known at
  // compile time. Asserting it means the encodes below cannot fail for capacity, which is why they assert rather
  // than handle.
  static constexpr std::size_t kFrameCapacity =
      wire::soupbintcp::kFrameOverhead + wire::ouch::kMaxMessageBytes;
  static_assert(kFrameCapacity <= wire::soupbintcp::kMaxPacketBytes,
                "an acknowledgement must fit in one SoupBinTCP packet");

  constexpr order_session_writer() noexcept = default;
  explicit constexpr order_session_writer(std::uint64_t first_sequence) noexcept
      : sequence_(first_sequence) {}

  /** The sequence the *next* Sequenced Data Packet will carry: the convention Login Accepted uses. */
  [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept { return sequence_; }
  [[nodiscard]] constexpr std::uint64_t packets_out() const noexcept { return packets_; }
  [[nodiscard]] constexpr std::uint64_t acknowledgements_out() const noexcept { return acks_; }

  // Wraps one OUCH message in a numbered packet.
  //
  // This is the join the session exists for: the order host writes OUCH bytes and knows nothing about sequence
  // numbers, and this numbers them and knows nothing about orders.
  template <typename Emit>
  constexpr void sequenced(packet_view message, Emit&& emit) noexcept {
    std::array<std::byte, kFrameCapacity> out{};
    const mutable_packet_view view{out.data(), out.size()};
    std::size_t size = 0;
    const auto err =
        wire::soupbintcp::encode_packet(view, wire::soupbintcp::packet_type::sequenced_data, message)
            .get(size);
    // Unreachable by construction: kFrameCapacity is the largest acknowledgement plus the frame, and the
    // static_assert above proves it fits. Asserted rather than handled so a future OUCH message that breaks the
    // assumption stops the run instead of truncating a reply.
    DFR_ASSERT(err == error::ok, "an acknowledgement did not fit its own frame");
    ++sequence_;
    ++acks_;
    send(packet_view{out.data(), size}, emit);
  }

  template <typename Emit>
  [[nodiscard]] constexpr result<void> login_accepted(std::string_view session_id,
                                                      Emit&& emit) noexcept {
    std::array<std::byte, kFrameCapacity> out{};
    std::size_t size = 0;
    if (const auto err = wire::soupbintcp::encode_login_accepted(
                             mutable_packet_view{out.data(), out.size()}, session_id, sequence_)
                             .get(size);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    send(packet_view{out.data(), size}, emit);
    return ok();
  }

  template <typename Emit>
  constexpr void login_rejected(wire::soupbintcp::reject_reason reason, Emit&& emit) noexcept {
    std::array<std::byte, kFrameCapacity> out{};
    std::size_t size = 0;
    const auto err = wire::soupbintcp::encode_login_rejected(
                         mutable_packet_view{out.data(), out.size()}, reason)
                         .get(size);
    DFR_ASSERT(err == error::ok, "a login rejection did not fit its own frame");
    send(packet_view{out.data(), size}, emit);
  }

  template <typename Emit>
  constexpr void bare(wire::soupbintcp::packet_type type, Emit&& emit) noexcept {
    std::array<std::byte, wire::soupbintcp::kFrameOverhead> out{};
    std::size_t size = 0;
    const auto err =
        wire::soupbintcp::encode_bare(mutable_packet_view{out.data(), out.size()}, type).get(size);
    DFR_ASSERT(err == error::ok, "an empty packet did not fit its own frame");
    send(packet_view{out.data(), size}, emit);
  }

  /** Adopts the position a login answered with, for a session resumed rather than started. */
  constexpr void restart_at(std::uint64_t sequence) noexcept { sequence_ = sequence; }

 private:
  template <typename Emit>
  constexpr void send(packet_view frame, Emit&& emit) noexcept {
    ++packets_;
    emit(frame);
  }

  std::uint64_t sequence_{1};
  std::uint64_t packets_{0};
  std::uint64_t acks_{0};
};

}  // namespace dfr::inline v1::venue
#endif  // DFR_VENUE_ORDER_SESSION_WRITER_HPP

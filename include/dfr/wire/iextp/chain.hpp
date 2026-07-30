// The three-chain consistency check.
//
// This is the reason to build against IEX first. Two of the three chains are
// redundant with the third, and redundancy is what turns a decoder into an
// oracle: a corrupted Payload Length is invisible to a sequence-number check and
// vice versa.

#ifndef DFR_WIRE_IEXTP_CHAIN_HPP
#define DFR_WIRE_IEXTP_CHAIN_HPP

#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/iextp/cursor.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace wire::iextp {

// ---------------------------------------------------------------------------
// The three-chain check
// ---------------------------------------------------------------------------

// Tracks what consecutive packets on one channel must satisfy.
//
// This is the reason to build against IEX first. Two of these three chains are
// redundant with the third, and redundancy is what turns a decoder into an
// oracle: a corrupted Payload Length is invisible to a sequence-number check
// and vice versa, so a receiver that verifies both detects a class of fault that
// neither alone can see.
class chain_checker {
 public:
  constexpr chain_checker() noexcept = default;

  // Feeds one packet's header. Returns the first inconsistency found, if any.
  //
  // The first packet establishes the chain rather than being checked against
  // nothing, so a receiver that joins a live feed mid-session does not report a
  // spurious error on its first packet.
  [[nodiscard]] constexpr result<void> observe(const header& value) noexcept {
    if (!started_) {
      started_ = true;
      session_ = value.session;
      expected_sequence_ = value.next_sequence();
      expected_offset_ = value.next_stream_offset();
      return ok();
    }

    if (value.session != session_) DFR_UNLIKELY {
      // Fatal: every sequence number and stream offset held refers to the old
      // session.
      session_ = value.session;
      expected_sequence_ = value.next_sequence();
      expected_offset_ = value.next_stream_offset();
      return error::session_changed;
    }

    if (value.first_sequence != expected_sequence_) DFR_UNLIKELY {
      const bool behind = value.first_sequence < expected_sequence_;
      // Resynchronise either way, so one gap does not produce an error on every
      // subsequent packet.
      expected_sequence_ = value.next_sequence();
      expected_offset_ = value.next_stream_offset();
      return behind ? error::sequence_regressed : error::sequence_gap;
    }

    if (value.stream_offset != expected_offset_) DFR_UNLIKELY {
      // Sequence numbers chained correctly and byte offsets did not. Neither
      // check alone would have found this.
      expected_offset_ = value.next_stream_offset();
      return error::message_length_mismatch;
    }

    expected_sequence_ = value.next_sequence();
    expected_offset_ = value.next_stream_offset();
    return ok();
  }

  [[nodiscard]] constexpr bool started() const noexcept { return started_; }
  [[nodiscard]] constexpr std::uint64_t expected_sequence() const noexcept {
    return expected_sequence_;
  }
  [[nodiscard]] constexpr std::int64_t expected_stream_offset() const noexcept {
    return expected_offset_;
  }

 private:
  std::uint64_t expected_sequence_{0};
  std::int64_t expected_offset_{0};
  std::uint32_t session_{0};
  bool started_{false};
};

// The third chain, checked within one packet rather than across two: the block
// framing must account for exactly the declared payload length.
//
// Separate from chain_checker because it needs the payload bytes, not just the
// header, and because a caller that only reads headers should still be able to
// use the cross-packet chains.
[[nodiscard]] constexpr result<void> verify_payload_framing(
    packet_view packet) noexcept {
  message_cursor cursor;
  if (const auto err = message_cursor::over(packet).get(cursor);
      err != error::ok) DFR_UNLIKELY {
    return err;
  }
  return cursor.drain([](const message&) {});
}

}  // namespace wire::iextp
}  // namespace dfr::inline v1

#endif  // DFR_WIRE_IEXTP_CHAIN_HPP

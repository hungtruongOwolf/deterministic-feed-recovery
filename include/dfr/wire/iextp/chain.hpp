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
  // Detection and resynchronisation are deliberately separate: inconsistency() is
  // const and decides nothing else, and the expectation is then rebuilt from this
  // packet unconditionally, on every path, exactly once. That structure carries an
  // invariant the rest of the library depends on — **a report is a statement about
  // one adjacent pair of packets and nothing more** — which is what lets
  // tests/chaos/oracle_test.cpp count breaks independently and demand the numbers
  // match.
  //
  // Writing it as four returns, each doing its own resynchronisation, is how this
  // was written first, and one of the four forgot the sequence expectation. The
  // consequence was a spurious gap on the packet *after* an offset break — blamed
  // on a packet that was perfectly correct, and enough to make a receiver ask for a
  // retransmit it did not need. Structure, not vigilance.
  [[nodiscard]] constexpr result<void> observe(const header& value) noexcept {
    const error found = inconsistency(value);

    // The first packet establishes the chain rather than being checked against
    // nothing, so a receiver joining a live feed mid-session does not report a
    // spurious error on its first packet. Resynchronising after a break serves the
    // same end: one gap must not produce an error on every packet that follows.
    started_ = true;
    session_ = value.session;
    expected_sequence_ = value.next_sequence();
    expected_offset_ = value.next_stream_offset();

    if (found != error::ok) DFR_UNLIKELY {
      return found;
    }
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
  // Which chain, if any, this packet breaks. Const and side-effect free, so the
  // three rules can be read as three rules.
  //
  // The order matters and is not arbitrary. A session change is checked first
  // because it makes both other checks meaningless: every sequence number and
  // stream offset held refers to the old session. The offset check comes last
  // because it is the one with no counterpart in MoldUDP64 — reaching it means the
  // sequence numbers chained perfectly and the byte positions did not, which is
  // what a corrupted Payload Length looks like and what neither check alone can
  // see.
  [[nodiscard]] constexpr error inconsistency(const header& value) const noexcept {
    if (!started_) {
      return error::ok;
    }
    if (value.session != session_) DFR_UNLIKELY {
      return error::session_changed;
    }
    if (value.first_sequence != expected_sequence_) DFR_UNLIKELY {
      return value.first_sequence < expected_sequence_
                 ? error::sequence_regressed
                 : error::sequence_gap;
    }
    if (value.stream_offset != expected_offset_) DFR_UNLIKELY {
      return error::message_length_mismatch;
    }
    return error::ok;
  }

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

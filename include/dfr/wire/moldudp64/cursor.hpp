// Walking the message blocks of one MoldUDP64 datagram.
//
// This is where the penberg/helix defect is refused: the cursor never trusts the
// block count the header declares.

#ifndef DFR_WIRE_MOLDUDP64_CURSOR_HPP
#define DFR_WIRE_MOLDUDP64_CURSOR_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/moldudp64/constants.hpp>
#include <dfr/wire/moldudp64/header.hpp>

#include <cstdint>

namespace dfr::inline v1::wire::moldudp64 {

// One message, with the sequence number it occupies.
struct message {
  std::uint64_t sequence{0};
  packet_view payload{};

  [[nodiscard]] friend bool operator==(const message&,
                                       const message&) = default;
};

// Walks the message blocks of one datagram.
//
// An explicit cursor rather than a callback. helix and most implementations take
// a handler, which forces control flow to invert and makes it awkward to stop
// half way, which is exactly what a fault injector and a differential test
// both need to do. TIGER_STYLE's "centralize control flow" points the same way:
// the loop belongs to the caller.
//
// The cursor never trusts the header's count. Both of the ways that count can
// lie are detected:
//
//   * more blocks claimed than bytes remain      -> block_count_overstated
//   * a block's own length runs past the end     -> block_overruns_datagram
//
// This is the pair of defects in penberg/helix (moldudp64.hh:106-115).
class message_cursor {
 public:
  // A cursor over nothing: exhausted, with no messages and no bytes left.
  //
  // Required because result<message_cursor> stores the value beside the error
  // and so needs a default-constructible T: the trade documented in
  // result.hpp. It costs nothing here because the state is genuinely
  // meaningful rather than an "invalid" sentinel: done() is true, remaining()
  // is zero, rest() is empty, and next() asserts exactly as it does on any
  // other exhausted cursor. There is no state a caller can reach through this
  // constructor that is not also reachable from a heartbeat packet.
  constexpr message_cursor() noexcept = default;

  // `packet` is the whole datagram, header included. Taking the whole thing
  // rather than a pre-sliced payload means a caller cannot accidentally pass a
  // payload sliced with the wrong offset.
  [[nodiscard]] static constexpr result<message_cursor> over(
      packet_view packet) noexcept {
    header decoded;
    if (const auto err = decode_header(packet).get(decoded);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }

    packet_view payload{};
    if (const auto err = packet.suffix(kHeaderSize).get(payload);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }

    return message_cursor{decoded, payload};
  }

  [[nodiscard]] constexpr const header& packet_header() const noexcept {
    return header_;
  }

  // Blocks the header still claims are present.
  //
  // Zero for a heartbeat and for end-of-session, so the ordinary
  // `while (!done())` loop handles both without a special case.
  [[nodiscard]] constexpr std::uint16_t remaining() const noexcept {
    return remaining_;
  }

  [[nodiscard]] constexpr bool done() const noexcept {
    return remaining_ == 0;
  }

  // Bytes not yet consumed. A caller should check this is empty once done(),
  // which is what catches a header that under-states its count.
  [[nodiscard]] constexpr packet_view rest() const noexcept { return payload_; }

  [[nodiscard]] constexpr result<message> next() noexcept {
    DFR_ASSERT(!done(), "next() on an exhausted cursor; check done() first");

    packet_view length_field;
    if (const auto err = payload_.prefix(kMessageLengthSize).get(length_field);
        err != error::ok) DFR_UNLIKELY {
      // The header claimed another block and there are not even two bytes left
      // for its length. The count was a lie.
      return error::block_count_overstated;
    }
    const std::uint16_t length = length_field.be16_at(0);

    if (const auto err = payload_.consume(kMessageLengthSize); !err)
        DFR_UNLIKELY {
      return err.error_code();
    }

    packet_view body;
    if (const auto err = payload_.prefix(length).get(body); err != error::ok)
        DFR_UNLIKELY {
      // The length field itself is the lie this time.
      return error::block_overruns_datagram;
    }
    if (const auto err = payload_.consume(length); !err) DFR_UNLIKELY {
      return err.error_code();
    }

    // A zero-length block is legal on the wire, and is not the end-of-session
    // marker: that is signalled by Message Count, not by a length. Recorded
    // rather than asserted, because it is a state a correct publisher may
    // produce and a reader should not be surprised by it.
    DFR_MAYBE(length == 0);

    const std::uint64_t sequence = next_sequence_;
    ++next_sequence_;
    --remaining_;
    return message{.sequence = sequence, .payload = body};
  }

  // Consumes everything and reports the first problem, if any.
  //
  // The convenience wrapper for the common case, built on the cursor rather
  // than replacing it. Also checks for trailing bytes, which a caller driving
  // the cursor by hand can forget.
  template <typename Handler>
  [[nodiscard]] constexpr result<void> drain(Handler&& handler) noexcept {
    while (!done()) {
      message current;
      if (const auto err = next().get(current); err != error::ok) DFR_UNLIKELY {
        return err;
      }
      handler(current);
    }
    if (!payload_.empty()) DFR_UNLIKELY {
      // Every block the header accounted for was consumed and bytes remain.
      // The count under-states, which loses data just as surely as over-stating
      // reads garbage.
      return error::trailing_bytes;
    }
    return ok();
  }

 private:
  // The only way to get a populated cursor is through over(), which validates.
  constexpr message_cursor(header decoded, packet_view payload) noexcept
      : header_(decoded),
        payload_(payload),
        next_sequence_(decoded.sequence),
        // A heartbeat and an end-of-session packet carry no blocks, so the
        // cursor is born exhausted and no caller needs to special-case them.
        remaining_(decoded.kind() == packet_kind::data ? decoded.message_count
                                                       : std::uint16_t{0}) {}

  header header_;
  packet_view payload_;
  // In-class defaults, so the defaulted constructor above actually delivers what its own comment promises.
  // Found by clang-tidy, and worth stating precisely: without these, the sibling constructor's initializer
  // list masked it everywhere the class is normally built, and only a default-constructed cursor (the one
  // path with no initializer list at all) would ever read the indeterminate value. `wire::iextp::cursor`
  // already carries the same two fields with `{0}`; this one had drifted from that pattern.
  std::uint64_t next_sequence_{0};
  std::uint16_t remaining_{0};
};

}  // namespace dfr::inline v1::wire::moldudp64
#endif  // DFR_WIRE_MOLDUDP64_CURSOR_HPP

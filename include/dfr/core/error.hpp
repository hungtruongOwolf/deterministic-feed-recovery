// Error codes.
//
// The design rule, from docs/DESIGN.md section 2: malformed wire data is not an
// exception and not an assertion. It is the *product* of this library, so it is
// reported as a value.
//
// Two properties are copied deliberately from simdjson:
//
//   - `ok` is zero, so `if (err)` reads correctly and the success path needs no
//     comparison against a named constant.
//   - `is_fatal()` exists, because the important distinction is not how severe
//     an error looks but whether the stream can continue. A sequence gap is
//     routine. A session-id change invalidates every byte of accumulated state.
//
// The enum will grow as components land. It is not ABI-stable before 1.0.

#ifndef DFR_CORE_ERROR_HPP
#define DFR_CORE_ERROR_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {

// Grouped by the layer that produces the error, because the caller's response
// differs by group: a framing error means discard this datagram, a sequencing
// error means enter recovery, a usage error means fix the caller.
enum class error : std::uint8_t {
  // ---- success ------------------------------------------------------------
  ok = 0,

  // ---- framing: this datagram is unusable, the stream is not -------------
  truncated_header,
  truncated_block,
  // The header's block count claims more blocks than the datagram contains.
  // This is the defect in penberg/helix (moldudp64.hh:106-115), which trusts
  // the count and walks off the buffer. It is a first-class fault op for
  // dfr::chaos precisely because a real implementation got it wrong.
  block_count_overstated,
  // A block's own length field would read past the end of the datagram.
  block_overruns_datagram,
  // Bytes remain after the last block the header accounted for.
  trailing_bytes,
  unknown_message_type,
  // A message whose declared length disagrees with the fixed size its type
  // mandates.
  message_length_mismatch,

  // ---- sequencing: expected, and the reason this library exists ----------
  // Messages were missed. Recoverable, and the normal case.
  sequence_gap,
  // A sequence number at or below one already delivered: a duplicate, or the
  // late half of an A/B pair. Recoverable and usually uninteresting.
  sequence_regressed,
  // The publisher's sequence numbering restarted. Whether this is fatal
  // depends on whether a session change accompanied it, which is why it is
  // distinct from session_changed.
  sequence_reset,
  // The session identifier changed mid-stream. Every byte of accumulated state
  // belongs to the old session and must be discarded.
  session_changed,
  end_of_session,

  // ---- recovery: the repair attempt itself failed ------------------------
  // The requested range is no longer in the publisher's retention window. The
  // gap is now unrecoverable by retransmission; a snapshot is required.
  retransmit_window_exceeded,
  retransmit_rejected,
  retransmit_timed_out,
  // A snapshot arrived whose sequence number is behind the oldest message
  // still buffered, so the range between them can never be filled. This is the
  // Glimpse race described in BUILD-GUIDE.md section 4, and it is the failure
  // mode that silently produces a plausible but permanently wrong book.
  snapshot_behind_buffer,
  snapshot_stale,
  // The bounded buffer held during recovery filled before the snapshot
  // completed. Detecting this is what separates a correct client from one that
  // silently loses the range it could not hold.
  recovery_buffer_overflow,

  // ---- usage: a programmer error that is reported rather than asserted ---
  // Reserved for boundaries where a value must be returned because the caller
  // is outside our control, for example a command-line argument. Inside the
  // library, prefer an assertion.
  invalid_argument,
  capacity_exceeded,
  not_supported,

  // Not an error. Keep last; used to size tables and to bound iteration.
  count_
};

// The number of enumerators, for table sizing. Named without a `k` prefix in
// the style of simdjson's NUM_ERROR_CODES.
inline constexpr auto kErrorCount = static_cast<std::size_t>(error::count_);

// Whether the stream can continue without re-establishing state from scratch.
//
// Deliberately not "is this bad". A sequence gap is not fatal even though it
// means data was lost, because retransmission or a snapshot repairs it. A
// session change is fatal even though nothing was corrupted, because every
// sequence number the client holds now refers to a different stream.
[[nodiscard]] DFR_FLATTEN_INLINE constexpr bool is_fatal(error err) noexcept {
  switch (err) {
    case error::session_changed:
    case error::snapshot_behind_buffer:
    case error::retransmit_window_exceeded:
    case error::recovery_buffer_overflow:
    case error::end_of_session:
      return true;

    case error::ok:
    case error::truncated_header:
    case error::truncated_block:
    case error::block_count_overstated:
    case error::block_overruns_datagram:
    case error::trailing_bytes:
    case error::unknown_message_type:
    case error::message_length_mismatch:
    case error::sequence_gap:
    case error::sequence_regressed:
    case error::sequence_reset:
    case error::retransmit_rejected:
    case error::retransmit_timed_out:
    case error::snapshot_stale:
    case error::invalid_argument:
    case error::capacity_exceeded:
    case error::not_supported:
      return false;

    case error::count_:
      break;
  }
  // Every enumerator is listed above rather than using a default, so that
  // adding one without classifying it is a compiler warning under
  // -Wswitch rather than a silent "not fatal". That works because
  // this switch has no default label.
  return false;
}

// Whether the error describes a damaged datagram rather than a stream-level
// condition. A caller that only wants to count bad packets can use this
// instead of listing enumerators.
[[nodiscard]] DFR_FLATTEN_INLINE constexpr bool is_framing_error(
    error err) noexcept {
  return err >= error::truncated_header && err <= error::message_length_mismatch;
}

// Whether the error is a sequencing observation. These are the expected output
// of a healthy client watching an unhealthy feed.
[[nodiscard]] DFR_FLATTEN_INLINE constexpr bool is_sequencing_error(
    error err) noexcept {
  return err >= error::sequence_gap && err <= error::end_of_session;
}

// A stable, allocation-free name. Returns the enumerator spelling rather than
// prose, so that log output can be grepped against this header.
[[nodiscard]] constexpr std::string_view to_string(error err) noexcept {
  switch (err) {
    case error::ok:                         return "ok";
    case error::truncated_header:           return "truncated_header";
    case error::truncated_block:            return "truncated_block";
    case error::block_count_overstated:     return "block_count_overstated";
    case error::block_overruns_datagram:    return "block_overruns_datagram";
    case error::trailing_bytes:             return "trailing_bytes";
    case error::unknown_message_type:       return "unknown_message_type";
    case error::message_length_mismatch:    return "message_length_mismatch";
    case error::sequence_gap:               return "sequence_gap";
    case error::sequence_regressed:         return "sequence_regressed";
    case error::sequence_reset:             return "sequence_reset";
    case error::session_changed:            return "session_changed";
    case error::end_of_session:             return "end_of_session";
    case error::retransmit_window_exceeded: return "retransmit_window_exceeded";
    case error::retransmit_rejected:        return "retransmit_rejected";
    case error::retransmit_timed_out:       return "retransmit_timed_out";
    case error::snapshot_behind_buffer:     return "snapshot_behind_buffer";
    case error::snapshot_stale:             return "snapshot_stale";
    case error::recovery_buffer_overflow:   return "recovery_buffer_overflow";
    case error::invalid_argument:           return "invalid_argument";
    case error::capacity_exceeded:          return "capacity_exceeded";
    case error::not_supported:              return "not_supported";
    case error::count_:                     return "count_";
  }
  return "<unknown error>";
}

// A one-line explanation, for the rare place a human reads the output directly.
// Kept separate from to_string so that machine-readable and human-readable
// output never have to agree on a format.
[[nodiscard]] constexpr std::string_view describe(error err) noexcept {
  switch (err) {
    case error::ok:
      return "no error";
    case error::truncated_header:
      return "datagram is shorter than the transport header";
    case error::truncated_block:
      return "a message block's own header is cut short";
    case error::block_count_overstated:
      return "the header claims more blocks than the datagram contains";
    case error::block_overruns_datagram:
      return "a block's length field would read past the end of the datagram";
    case error::trailing_bytes:
      return "bytes remain after the last block the header accounted for";
    case error::unknown_message_type:
      return "message type is not in the protocol's set";
    case error::message_length_mismatch:
      return "declared length disagrees with the size the message type mandates";
    case error::sequence_gap:
      return "messages were missed; recovery is required";
    case error::sequence_regressed:
      return "sequence number is at or below one already delivered";
    case error::sequence_reset:
      return "the publisher restarted its sequence numbering";
    case error::session_changed:
      return "session identifier changed; accumulated state is invalid";
    case error::end_of_session:
      return "the publisher signalled end of session";
    case error::retransmit_window_exceeded:
      return "requested range has aged out of the publisher's retention window";
    case error::retransmit_rejected:
      return "the retransmission facility refused the request";
    case error::retransmit_timed_out:
      return "no retransmission response arrived within the deadline";
    case error::snapshot_behind_buffer:
      return "snapshot is older than the oldest buffered message; the range "
             "between them can never be filled";
    case error::snapshot_stale:
      return "snapshot is too old to be worth applying";
    case error::recovery_buffer_overflow:
      return "the recovery buffer filled before the snapshot completed";
    case error::invalid_argument:
      return "argument is outside its documented domain";
    case error::capacity_exceeded:
      return "a fixed-capacity structure is full";
    case error::not_supported:
      return "this build does not support the requested operation";
    case error::count_:
      return "not an error";
  }
  return "unknown error";
}

}  // namespace dfr::inline v1

#endif  // DFR_CORE_ERROR_HPP

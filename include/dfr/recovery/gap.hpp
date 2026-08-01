// A missing range of messages.
//
// The unit here is the *message* sequence number, not the packet. Both MoldUDP64
// and IEX-TP number messages rather than datagrams, and a gap is a statement about
// messages: "5 through 9 never arrived" is what a retransmit request has to say.
// Counting packets instead is a mistake that survives testing on a feed where every
// packet happens to carry one message, and IEX's heartbeats carry zero.
//
// Ranges are half-open, [first, end). The alternative: inclusive [first, last],
// matches how a specification reads, but makes the empty range unrepresentable
// except by convention, and every merge and split then needs a ±1 that is one
// keystroke from being wrong. Half-open costs one accessor, last(), used only when
// reporting to a human or building a wire request.

#ifndef DFR_RECOVERY_GAP_HPP
#define DFR_RECOVERY_GAP_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>

#include <cstdint>

namespace dfr::inline v1 {
namespace recovery {

// A half-open span of message sequence numbers.
//
// An empty range is representable (first == end) and is not an error: subtracting a
// retransmit that covered a whole gap produces one, and a caller that had to
// special-case that would be a caller with a bug waiting.
struct sequence_range {
  std::uint64_t first{0};
  std::uint64_t end{0};

  [[nodiscard]] constexpr bool empty() const noexcept { return first >= end; }

  [[nodiscard]] constexpr std::uint64_t count() const noexcept {
    return empty() ? 0 : end - first;
  }

  // The last sequence number in the range, for reporting and for wire requests
  // that are specified inclusively. Asserted rather than returning an optional,
  // because there is no sensible answer for an empty range and a caller asking is
  // a caller that has lost track.
  [[nodiscard]] constexpr std::uint64_t last() const noexcept {
    DFR_ASSERT(!empty(), "the last sequence of an empty range is meaningless");
    return end - 1;
  }

  [[nodiscard]] constexpr bool contains(std::uint64_t sequence) const noexcept {
    return sequence >= first && sequence < end;
  }

  // Whether this range holds every sequence number `other` does. An empty range is
  // covered by anything, including another empty range: the vacuous case, and the
  // one that makes fill() terminate cleanly.
  [[nodiscard]] constexpr bool covers(sequence_range other) const noexcept {
    if (other.empty()) {
      return true;
    }
    return !empty() && other.first >= first && other.end <= end;
  }

  [[nodiscard]] constexpr bool overlaps(sequence_range other) const noexcept {
    if (empty() || other.empty()) {
      return false;
    }
    return first < other.end && other.first < end;
  }

  // Whether the two ranges are contiguous with no hole between them, in either
  // order. Adjacent gaps are merged so that one lost burst is one retransmit
  // request rather than several; a receiver that sent one request per packet would
  // multiply its own recovery traffic by the burst length, which is the behaviour
  // NAK suppression exists to prevent.
  [[nodiscard]] constexpr bool adjacent_to(sequence_range other) const noexcept {
    if (empty() || other.empty()) {
      return false;
    }
    return end == other.first || other.end == first;
  }

  [[nodiscard]] friend constexpr bool operator==(sequence_range,
                                                 sequence_range) = default;
};

// The union of two ranges that touch. Asserted rather than checked, because
// unioning two ranges with a hole between them would silently claim the hole had
// arrived: the exact failure this library exists to catch.
[[nodiscard]] constexpr sequence_range merge(sequence_range a,
                                             sequence_range b) noexcept {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  DFR_ASSERT(a.overlaps(b) || a.adjacent_to(b),
             "merging disjoint ranges would claim the hole between them arrived");
  return sequence_range{.first = a.first < b.first ? a.first : b.first,
                        .end = a.end > b.end ? a.end : b.end};
}

// What removing `filled` from `hole` leaves behind.
//
// Two ranges out, not one, because a retransmit that lands in the middle of a gap
// splits it: asking for 5..9 and receiving only 7 leaves 5..6 and 8..9 still
// missing. A subtract that returned a single range would have to round outward and
// claim messages arrived that did not.
struct remainder {
  sequence_range before{};
  sequence_range after{};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return before.empty() && after.empty();
  }

  [[nodiscard]] constexpr std::uint64_t count() const noexcept {
    return before.count() + after.count();
  }
};

[[nodiscard]] constexpr remainder subtract(sequence_range hole,
                                           sequence_range filled) noexcept {
  if (hole.empty() || filled.empty() || !hole.overlaps(filled)) {
    return remainder{.before = hole, .after = sequence_range{}};
  }

  remainder out;
  if (filled.first > hole.first) {
    out.before = sequence_range{.first = hole.first, .end = filled.first};
  }
  if (filled.end < hole.end) {
    out.after = sequence_range{.first = filled.end, .end = hole.end};
  }
  return out;
}

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_GAP_HPP

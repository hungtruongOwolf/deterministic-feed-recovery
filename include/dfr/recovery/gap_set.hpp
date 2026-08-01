// The set of ranges a receiver is still missing on one channel.
//
// Kept in a canonical form that the class never leaves: sorted by `first`,
// non-overlapping, and with no two ranges adjacent. Canonical form is not tidiness
//: it is what makes "one lost burst is one retransmit request" true, and what lets
// oldest() answer in constant time. A receiver that sent one request per lost packet
// would multiply its own recovery traffic by the burst length, which is precisely
// the storm NAK suppression exists to prevent.
//
// Fixed capacity, and a fill can fail
// -----------------------------------
// Filling the *middle* of a gap splits one range into two, so a fill can need more
// capacity than it frees. When there is no room, it is refused and reported rather
// than resolved by merging the two halves: merging would claim the messages
// between them had arrived, which is the one lie this library exists to catch. A
// caller out of capacity has genuinely lost track of what it is missing and needs to
// hear so.

#ifndef DFR_RECOVERY_GAP_SET_HPP
#define DFR_RECOVERY_GAP_SET_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/gap.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dfr::inline v1::recovery {

// How many distinct holes one channel may be missing at once.
//
// Not a guess about traffic: it is a bound on how confused the receiver is willing
// to become. Sixteen separate outstanding holes on one channel means recovery is
// losing ground, and continuing to accumulate them silently would turn a detectable
// problem into a slow one.
inline constexpr std::size_t kMaxOutstandingGaps = 16;

class gap_set {
 public:
  constexpr gap_set() noexcept = default;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

  [[nodiscard]] constexpr std::span<const sequence_range> ranges()
      const noexcept DFR_LIFETIME_BOUND {
    return {ranges_.data(), count_};
  }

  // The lowest outstanding range: what recovery should ask for first, because it is
  // the one closest to falling out of the publisher's retention window.
  [[nodiscard]] constexpr sequence_range oldest() const noexcept {
    DFR_ASSERT(!empty(), "there is no oldest gap when nothing is missing");
    return ranges_[0];
  }

  [[nodiscard]] constexpr std::uint64_t total_missing() const noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      total += ranges_[i].count();
    }
    return total;
  }

  // The parts of `arrived` that this set is still missing.
  //
  // Exists because "is this packet a duplicate?" and "is this packet useful?" are
  // different questions, and the arbiter can only answer the first. A retransmit, or the
  // other line's copy arriving late, lands *below* the merged stream's watermark and looks
  // like a duplicate while being exactly what recovery was waiting for. The set of holes
  // is the only thing that can tell them apart.
  //
  // The result needs no capacity check: it can hold at most one range per range already
  // here, since an intersection cannot split anything.
  [[nodiscard]] constexpr gap_set intersect(sequence_range arrived) const noexcept {
    gap_set out;
    if (arrived.empty()) {
      return out;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      const sequence_range hole = ranges_[i];
      if (!hole.overlaps(arrived)) {
        continue;
      }
      out.ranges_[out.count_] = sequence_range{
          .first = hole.first > arrived.first ? hole.first : arrived.first,
          .end = hole.end < arrived.end ? hole.end : arrived.end};
      ++out.count_;
    }
    DFR_ASSERT_PARANOID(out.is_canonical(),
                        "intersect() produced a non-canonical set");
    return out;
  }

  // Records a newly discovered hole, merging it into anything it touches.
  //
  // An empty range is accepted and does nothing, so a caller computing a range
  // arithmetically does not have to check first.
  [[nodiscard]] constexpr result<void> open(sequence_range hole) noexcept {
    if (hole.empty()) {
      return ok();
    }

    // Absorb every range this one touches. Scanning the whole array rather than
    // stopping at the first miss: the new hole can bridge two existing ranges that
    // were not adjacent to each other, and stopping early would leave the set with
    // two ranges that are now adjacent, which is outside canonical form.
    sequence_range merged = hole;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const sequence_range existing = ranges_[i];
      if (existing.overlaps(merged) || existing.adjacent_to(merged)) {
        merged = merge(merged, existing);
      } else {
        ranges_[kept] = existing;
        ++kept;
      }
    }

    if (kept >= kMaxOutstandingGaps) DFR_UNLIKELY {
      // Nothing has been written past `kept`, and the surviving ranges were copied
      // down in order, so the set is still canonical and still describes exactly
      // what it did before minus nothing. Refusing without corrupting the set is
      // what lets a caller report and continue.
      count_ = kept;
      return error::capacity_exceeded;
    }

    count_ = kept;
    insert_sorted(merged);
    DFR_ASSERT_PARANOID(is_canonical(), "gap_set left canonical form in open()");
    return ok();
  }

  // Records that a range arrived, and says how many missing sequences that removed.
  //
  // The count is the useful return, not a bool: it is how a caller distinguishes a
  // retransmit that did its job from one that arrived duplicated or covered
  // sequences nobody was missing.
  [[nodiscard]] constexpr result<std::uint64_t> fill(
      sequence_range arrived) noexcept {
    if (arrived.empty() || empty()) {
      return std::uint64_t{0};
    }

    // Counted first, so the answer is right whether or not the rewrite below has
    // room to succeed.
    std::uint64_t removed = 0;
    std::size_t needed = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const remainder left = subtract(ranges_[i], arrived);
      removed += ranges_[i].count() - left.count();
      needed += static_cast<std::size_t>(!left.before.empty()) +
                static_cast<std::size_t>(!left.after.empty());
    }

    if (needed > kMaxOutstandingGaps) DFR_UNLIKELY {
      // A fill landing inside a gap splits it, so this is reachable. Refused rather
      // than merged: see the note at the top of this file.
      return error::capacity_exceeded;
    }

    // Rewritten in place and in order. Every output range derives from the input
    // range at the same index or the one before it, and subtract() preserves order,
    // so the result stays sorted without a sort.
    std::array<sequence_range, kMaxOutstandingGaps> next{};
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const remainder left = subtract(ranges_[i], arrived);
      if (!left.before.empty()) {
        next[kept] = left.before;
        ++kept;
      }
      if (!left.after.empty()) {
        next[kept] = left.after;
        ++kept;
      }
    }

    ranges_ = next;
    count_ = kept;
    DFR_ASSERT_PARANOID(is_canonical(), "gap_set left canonical form in fill()");
    return removed;
  }

  // Forgets everything below `sequence`, and says how much was abandoned.
  //
  // This is what a snapshot does: it re-establishes state at a sequence number, so
  // holes older than it can never be filled and no longer need to be. The count is
  // returned because abandoning a hole is data loss even when it is the correct
  // thing to do, and a caller that cannot report how much it gave up cannot be
  // audited.
  // Written out rather than expressed as subtract(hole, {0, sequence}).after: that
  // reads as if it should work and does not, because subtract() reports "no overlap"
  // by handing the whole hole back in `before`, so every range *above* `sequence`
  // would be silently dropped. A one-sided trim is not a subtraction, and saying so
  // directly is both shorter and correct.
  [[nodiscard]] constexpr std::uint64_t discard_below(
      std::uint64_t sequence) noexcept {
    std::uint64_t abandoned = 0;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      sequence_range hole = ranges_[i];
      if (hole.end <= sequence) {
        abandoned += hole.count();  // entirely unfillable now
        continue;
      }
      if (hole.first < sequence) {
        abandoned += sequence - hole.first;
        hole.first = sequence;
      }
      ranges_[kept] = hole;
      ++kept;
    }
    count_ = kept;
    DFR_ASSERT_PARANOID(is_canonical(),
                        "gap_set left canonical form in discard_below()");
    return abandoned;
  }

  constexpr void clear() noexcept { count_ = 0; }

 private:
  constexpr void insert_sorted(sequence_range value) noexcept {
    DFR_ASSERT(count_ < kMaxOutstandingGaps, "insert_sorted with no room");
    std::size_t at = 0;
    while (at < count_ && ranges_[at].first < value.first) {
      ++at;
    }
    for (std::size_t i = count_; i > at; --i) {
      ranges_[i] = ranges_[i - 1];
    }
    ranges_[at] = value;
    ++count_;
  }

  // The invariant, written once so the paranoid preset can check it after every
  // mutation instead of trusting four separate proofs.
  [[nodiscard]] constexpr bool is_canonical() const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (ranges_[i].empty()) {
        return false;
      }
      if (i > 0 && ranges_[i].first <= ranges_[i - 1].end) {
        return false;  // unsorted, overlapping, or adjacent
      }
    }
    return true;
  }

  std::array<sequence_range, kMaxOutstandingGaps> ranges_{};
  std::size_t count_{0};
};

}  // namespace dfr::inline v1::recovery
#endif  // DFR_RECOVERY_GAP_SET_HPP

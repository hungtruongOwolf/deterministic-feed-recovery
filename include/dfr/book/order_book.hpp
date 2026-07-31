// An aggregated order book, built from DEEP price level updates.
//
// This is what the message layer was for. Recovery could already prove "every sequence number arrived exactly
// once", which is a statement about bookkeeping. What a trading system needs is a statement about *content*:
// that the book it is looking at is the book that would have existed if nothing had been lost. That invariant
// needs a book, and a book needs the messages to mean something.
//
// Aggregated, not order-by-order
// ------------------------------
// DEEP publishes the *total size at a price*, not individual orders — so this holds one size per price and a
// level with size zero is a level that is gone. That is the protocol's model rather than a simplification, and
// getting it wrong the other way is a classic error: treating a size-zero update as "a level with no shares"
// leaves a phantom price in the book forever, and the book then quotes a bid nobody is offering.
//
// A sorted array, not a map
// -------------------------
// Levels are kept sorted, bids descending and asks ascending, so the best price is always at index zero and
// reading the top of book is one load rather than a tree walk. Insertion is a memmove, and for the depth a real
// feed publishes — tens of levels, not thousands — that beats a node-per-level structure on every access
// pattern, because the whole side fits in a cache line or two and a map does not.
//
// No allocation, ever
// -------------------
// Fixed capacity, like everything else here. A book that reallocated would have an allocation on the path that
// runs while the market is moving, which is the one place this project has decided that is not allowed. A level
// arriving beyond capacity is refused and counted, never silently dropped: a book that quietly stopped
// accepting depth would keep answering questions with an answer that used to be right.

#ifndef DFR_BOOK_ORDER_BOOK_HPP
#define DFR_BOOK_ORDER_BOOK_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/deep/messages.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dfr::inline v1 {
namespace book {

using wire::deep::price;

struct level {
  price at{};
  std::uint32_t size{0};

  [[nodiscard]] friend constexpr bool operator==(const level&, const level&) = default;
};

// One side of one book. Bids descend, asks ascend, and `Descending` is what makes the two the same code.
template <std::size_t Capacity, bool Descending>
class side {
 public:
  static_assert(Capacity >= 2, "a side needs room for at least two levels");

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  [[nodiscard]] constexpr std::span<const level> levels() const noexcept DFR_LIFETIME_BOUND {
    return {levels_.data(), count_};
  }

  /** The best price on this side, or a zero level when the side is empty. */
  [[nodiscard]] constexpr level best() const noexcept {
    return count_ == 0 ? level{} : levels_[0];
  }

  /** How many updates were refused for want of capacity. Non-zero means the book is incomplete and says so. */
  [[nodiscard]] constexpr std::uint64_t refused() const noexcept { return refused_; }

  /**
   * Applies one price level update.
   *
   * A size of zero removes the level, because that is what DEEP means by it. Everything else replaces the size
   * at that price, inserting the level if it is new.
   */
  constexpr result<void> apply(price at, std::uint32_t size) noexcept {
    const std::size_t where = position_of(at);
    const bool present = where < count_ && levels_[where].at == at;

    if (size == 0) {
      if (present) {
        remove_at(where);
      }
      // Removing a level that is not there is not an error. It happens legitimately on a feed joined mid-day,
      // and on a book rebuilt from a snapshot that did not include a level the next update deletes.
      return ok();
    }

    if (present) {
      levels_[where].size = size;
      return ok();
    }

    if (count_ >= Capacity) DFR_UNLIKELY {
      ++refused_;
      return error::capacity_exceeded;
    }
    insert_at(where, level{.at = at, .size = size});
    return ok();
  }

  constexpr void clear() noexcept {
    count_ = 0;
    // `refused_` survives, because it describes the book's history and a caller reporting that the book was
    // ever incomplete needs it after the reset that hid the evidence.
  }

  [[nodiscard]] friend constexpr bool operator==(const side& a, const side& b) noexcept {
    if (a.count_ != b.count_) {
      return false;
    }
    for (std::size_t i = 0; i < a.count_; ++i) {
      if (!(a.levels_[i] == b.levels_[i])) {
        return false;
      }
    }
    return true;
  }

 private:
  // Where `at` belongs, whether or not it is there. Linear because the depth is tens of levels and the array is
  // contiguous: a binary search over sixteen cache-resident entries loses to a scan the branch predictor learns.
  [[nodiscard]] constexpr std::size_t position_of(price at) const noexcept {
    std::size_t i = 0;
    while (i < count_ && better(levels_[i].at, at)) {
      ++i;
    }
    return i;
  }

  [[nodiscard]] static constexpr bool better(price a, price b) noexcept {
    return Descending ? a > b : a < b;
  }

  constexpr void insert_at(std::size_t where, level value) noexcept {
    DFR_ASSERT(count_ < Capacity, "inserting into a full side");
    for (std::size_t i = count_; i > where; --i) {
      levels_[i] = levels_[i - 1];
    }
    levels_[where] = value;
    ++count_;
  }

  constexpr void remove_at(std::size_t where) noexcept {
    DFR_ASSERT(where < count_, "removing a level that is not there");
    for (std::size_t i = where; i + 1 < count_; ++i) {
      levels_[i] = levels_[i + 1];
    }
    --count_;
  }

  std::array<level, Capacity> levels_{};
  std::size_t count_{0};
  std::uint64_t refused_{0};
};

// One symbol's book.
template <std::size_t Depth = 32>
class order_book {
 public:
  using bid_side = side<Depth, true>;
  using ask_side = side<Depth, false>;

  [[nodiscard]] constexpr const bid_side& bids() const noexcept DFR_LIFETIME_BOUND { return bids_; }
  [[nodiscard]] constexpr const ask_side& asks() const noexcept DFR_LIFETIME_BOUND { return asks_; }

  [[nodiscard]] constexpr std::uint64_t updates() const noexcept { return updates_; }
  [[nodiscard]] constexpr std::uint64_t trades() const noexcept { return trades_; }
  [[nodiscard]] constexpr std::uint64_t traded_shares() const noexcept { return traded_shares_; }

  /**
   * Whether the book is crossed — a bid at or above the best ask.
   *
   * Not an error and not corrected. A DEEP feed publishes one side at a time, so a book *is* briefly crossed
   * between the two halves of a quote change; that is why `price_level_update::event_complete()` exists. What
   * would be a defect is a book that stayed crossed after an event completed, and a caller checking that needs
   * this to be observable rather than silently repaired.
   */
  [[nodiscard]] constexpr bool crossed() const noexcept {
    return !bids_.empty() && !asks_.empty() && bids_.best().at >= asks_.best().at;
  }

  constexpr result<void> apply(const wire::deep::price_level_update& update) noexcept {
    ++updates_;
    return update.buy ? bids_.apply(update.level, update.size)
                      : asks_.apply(update.level, update.size);
  }

  // Trades do not change an aggregated book: the size reduction arrives as its own price level update. Counted
  // rather than applied, because a caller reconciling volume needs the count and a book that also decremented
  // a level would double-count every fill.
  constexpr void observe(const wire::deep::trade_report& trade) noexcept {
    if (trade.broken) {
      ++broken_trades_;
      return;
    }
    ++trades_;
    traded_shares_ += trade.size;
  }

  [[nodiscard]] constexpr std::uint64_t broken_trades() const noexcept { return broken_trades_; }

  constexpr void clear() noexcept {
    bids_.clear();
    asks_.clear();
  }

  // Two books are equal when both sides are. This is the operator the recovery oracle turns on: the book after
  // loss and repair must equal the book that never lost anything.
  [[nodiscard]] friend constexpr bool operator==(const order_book& a, const order_book& b) noexcept {
    return a.bids_ == b.bids_ && a.asks_ == b.asks_;
  }

 private:
  bid_side bids_{};
  ask_side asks_{};
  std::uint64_t updates_{0};
  std::uint64_t trades_{0};
  std::uint64_t traded_shares_{0};
  std::uint64_t broken_trades_{0};
};

}  // namespace book
}  // namespace dfr::inline v1

#endif  // DFR_BOOK_ORDER_BOOK_HPP

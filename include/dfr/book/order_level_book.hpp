// A fixed-capacity order-by-order book for Nasdaq ITCH lifecycle messages.
//
// The aggregated DEEP book can prove that price levels survive recovery. This book adds identity:
// an execution changes one order, a replace retires one reference and creates another, and a delete
// must remove the exact live order named by the feed. Those are the semantics an order-level recovery
// oracle needs and a price-level book cannot express.

#ifndef DFR_BOOK_ORDER_LEVEL_BOOK_HPP
#define DFR_BOOK_ORDER_LEVEL_BOOK_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/itch/messages.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::book {

struct order {
  std::uint64_t reference{0};
  wire::itch::stock stock{};
  std::uint32_t shares{0};
  std::uint32_t price{0};
  bool buy{false};

  [[nodiscard]] friend constexpr bool operator==(const order&, const order&) = default;
};

struct order_level_stats {
  std::uint64_t added{0};
  std::uint64_t executed{0};
  std::uint64_t canceled{0};
  std::uint64_t deleted{0};
  std::uint64_t replaced{0};
  std::uint64_t executed_shares{0};
};

template <std::size_t Capacity = 4096>
class order_level_book {
 public:
  static_assert(Capacity >= 8, "an order book needs useful collision space");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "capacity must be a power of two for deterministic open addressing");

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
  [[nodiscard]] constexpr const order_level_stats& stats() const noexcept { return stats_; }

  [[nodiscard]] constexpr const order* find(std::uint64_t reference) const noexcept
      DFR_LIFETIME_BOUND {
    const std::size_t at = find_index(reference);
    return at == Capacity ? nullptr : &slots_[at].value;
  }

  [[nodiscard]] constexpr std::uint64_t shares_open() const noexcept {
    std::uint64_t total = 0;
    for (const slot& entry : slots_) {
      if (entry.state == slot_state::live) {
        total += entry.value.shares;
      }
    }
    return total;
  }

  [[nodiscard]] constexpr result<void> apply(const wire::itch::add_order& message) noexcept {
    if (message.order_reference == 0 || message.shares == 0 ||
        find(message.order_reference) != nullptr) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    const order value{.reference = message.order_reference,
                      .stock = message.symbol,
                      .shares = message.shares,
                      .price = message.price,
                      .buy = message.buy};
    if (const auto inserted = insert(value); !inserted) DFR_UNLIKELY {
      return inserted;
    }
    ++stats_.added;
    return ok();
  }

  [[nodiscard]] constexpr result<void> apply(
      const wire::itch::order_executed& message) noexcept {
    order* existing = mutable_find(message.order_reference);
    if (existing == nullptr || message.shares == 0 ||
        message.shares > existing->shares) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    existing->shares -= message.shares;
    if (existing->shares == 0) {
      erase(message.order_reference);
    }
    ++stats_.executed;
    stats_.executed_shares += message.shares;
    return ok();
  }

  [[nodiscard]] constexpr result<void> apply(const wire::itch::order_cancel& message) noexcept {
    order* existing = mutable_find(message.order_reference);
    if (existing == nullptr || message.shares == 0 ||
        message.shares > existing->shares) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    existing->shares -= message.shares;
    if (existing->shares == 0) {
      erase(message.order_reference);
    }
    ++stats_.canceled;
    return ok();
  }

  [[nodiscard]] constexpr result<void> apply(const wire::itch::order_delete& message) noexcept {
    if (find(message.order_reference) == nullptr) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    erase(message.order_reference);
    ++stats_.deleted;
    return ok();
  }

  [[nodiscard]] constexpr result<void> apply(const wire::itch::order_replace& message) noexcept {
    const order* existing = find(message.original_reference);
    if (existing == nullptr || message.new_reference == 0 || message.shares == 0 ||
        message.new_reference == message.original_reference ||
        find(message.new_reference) != nullptr) DFR_UNLIKELY {
      return error::invalid_argument;
    }

    // Copy before erase: open addressing may reuse the same slot for the new reference.
    order replacement = *existing;
    replacement.reference = message.new_reference;
    replacement.shares = message.shares;
    replacement.price = message.price;
    erase(message.original_reference);
    const auto inserted = insert(replacement);
    DFR_ASSERT(inserted.has_value(), "a replace frees one slot before inserting one slot");
    if (!inserted) DFR_UNLIKELY {
      return inserted;
    }
    ++stats_.replaced;
    return ok();
  }

  [[nodiscard]] friend constexpr bool operator==(const order_level_book& a,
                                                 const order_level_book& b) noexcept {
    if (a.count_ != b.count_ || a.stats_.executed_shares != b.stats_.executed_shares) {
      return false;
    }
    for (const slot& entry : a.slots_) {
      if (entry.state == slot_state::live) {
        const order* other = b.find(entry.value.reference);
        if (other == nullptr || !(*other == entry.value)) {
          return false;
        }
      }
    }
    return true;
  }

 private:
  enum class slot_state : std::uint8_t { empty, live, tombstone };

  struct slot {
    order value{};
    slot_state state{slot_state::empty};
  };

  [[nodiscard]] static constexpr std::size_t bucket(std::uint64_t reference) noexcept {
    // Fibonacci hashing: fixed, cheap and independent of the standard library implementation.
    return static_cast<std::size_t>((reference * 11400714819323198485ULL) & (Capacity - 1));
  }

  [[nodiscard]] constexpr std::size_t find_index(std::uint64_t reference) const noexcept {
    const std::size_t start = bucket(reference);
    for (std::size_t distance = 0; distance < Capacity; ++distance) {
      const std::size_t at = (start + distance) & (Capacity - 1);
      if (slots_[at].state == slot_state::empty) {
        return Capacity;
      }
      if (slots_[at].state == slot_state::live &&
          slots_[at].value.reference == reference) {
        return at;
      }
    }
    return Capacity;
  }

  [[nodiscard]] constexpr order* mutable_find(std::uint64_t reference) noexcept {
    const std::size_t at = find_index(reference);
    return at == Capacity ? nullptr : &slots_[at].value;
  }

  [[nodiscard]] constexpr result<void> insert(order value) noexcept {
    if (count_ >= Capacity) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }
    const std::size_t start = bucket(value.reference);
    std::size_t first_tombstone = Capacity;
    for (std::size_t distance = 0; distance < Capacity; ++distance) {
      const std::size_t at = (start + distance) & (Capacity - 1);
      if (slots_[at].state == slot_state::tombstone && first_tombstone == Capacity) {
        first_tombstone = at;
      }
      if (slots_[at].state == slot_state::empty) {
        const std::size_t into = first_tombstone == Capacity ? at : first_tombstone;
        slots_[into] = slot{.value = value, .state = slot_state::live};
        ++count_;
        return ok();
      }
    }
    if (first_tombstone != Capacity) {
      slots_[first_tombstone] = slot{.value = value, .state = slot_state::live};
      ++count_;
      return ok();
    }
    return error::capacity_exceeded;
  }

  constexpr void erase(std::uint64_t reference) noexcept {
    const std::size_t at = find_index(reference);
    DFR_ASSERT(at < Capacity, "erasing an order that is not live");
    slots_[at].state = slot_state::tombstone;
    --count_;
  }

  std::array<slot, Capacity> slots_{};
  std::size_t count_{0};
  order_level_stats stats_{};
};

}  // namespace dfr::inline v1::book
#endif  // DFR_BOOK_ORDER_LEVEL_BOOK_HPP

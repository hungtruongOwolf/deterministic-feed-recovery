// Channel identity: a small integer assigned once, at configuration time.
//
// This exists because of a specific defect in a real library. `bbalouki/itchcpp`
// constructs `const std::string key{session}` **on every packet** to index an
// `unordered_map<std::string, uint64_t>` (include/itch/transport/sequencing.hpp:86-96),
// and repeats it in expected_next() (:138), one heap allocation and a hash per
// packet, on the hot path, to answer a question whose answer was fixed before the
// feed opened. docs/DESIGN.md §0 records the consequence for us: no map, no string,
// no std::function.
//
// A linear scan over at most eight entries replaces the hash. That is not a
// compromise: eight u32 comparisons over one cache line beat a hash, a modulo and a
// pointer chase, and they allocate nothing. The scan happens once per *packet*, not
// per message.
//
// Two integers, kept apart by the type system
// -------------------------------------------
// The wire carries a 32-bit channel number chosen by the venue; internally a channel
// is an index into fixed-size arrays. Confusing them means using a venue's number:
// IEX's observed values run into the thousands: as an array index, which is a
// silent out-of-bounds read on a hot path. `channel_id` therefore cannot be
// constructed from an arbitrary integer or converted back to one implicitly.

#ifndef DFR_RECOVERY_CHANNEL_HPP
#define DFR_RECOVERY_CHANNEL_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace recovery {

// How many channels one tracker follows. IEX DEEP publishes one; CME MDP 3.0
// separates incremental, snapshot and instrument-definition channels per group.
// Eight is room for a realistic multi-channel venue and small enough that the
// per-channel state stays in a handful of cache lines.
inline constexpr std::size_t kMaxChannels = 8;

// An index into the tracker's per-channel arrays. Deliberately not an integer.
class channel_id {
 public:
  // Defaults to channel 0, and that is a concession worth stating plainly.
  //
  // Deleting the default constructor is tempting: channel 0 is a real channel, so a
  // forgotten initialisation would quietly attribute one feed's sequence numbers to
  // another. But `result<T>` stores its value and its error side by side rather than
  // in a variant, deliberately, following simdjson_result, so it requires a
  // default-initializable T. Bending a brand-new type to a load-bearing decision made
  // across the whole library is the right way round; bending result<T> for one type is
  // not.
  //
  // What the type still prevents is the failure that actually happens: a venue's
  // channel *number* being used as an array index. Those run into the thousands, the
  // constructor taking one is private, and at() is a checked conversion that happens
  // once at configuration time. Forgetting to initialise a local is a different and
  // much shallower mistake, and `-Wuninitialized` is a better tool for it than a
  // deleted constructor that would distort the API.
  constexpr channel_id() noexcept = default;

  [[nodiscard]] static constexpr channel_id at(std::size_t index) noexcept {
    DFR_ASSERT(index < kMaxChannels, "channel index out of range");
    return channel_id{static_cast<std::uint8_t>(index)};
  }

  [[nodiscard]] constexpr std::size_t index() const noexcept { return value_; }

  [[nodiscard]] friend constexpr bool operator==(channel_id,
                                                 channel_id) = default;

 private:
  explicit constexpr channel_id(std::uint8_t value) noexcept : value_(value) {}

  std::uint8_t value_{0};
};

// Maps the venue's channel number to an internal index.
//
// Populated during configuration and then read-only, which is what makes the linear
// scan safe to call per packet: the table cannot grow while the feed is running, so
// its cost is bounded by a number the operator chose.
class channel_table {
 public:
  constexpr channel_table() noexcept = default;

  // Registers a channel number and returns the id assigned to it.
  //
  // Registering the same number twice returns the id it already has rather than
  // failing. A configuration that lists a channel under two names is describing one
  // channel, and refusing would make the caller special-case something harmless;
  // handing out a second id would split one feed's state in two, which is not
  // harmless at all.
  [[nodiscard]] constexpr result<channel_id> add(
      std::uint32_t wire_channel) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (numbers_[i] == wire_channel) {
        return channel_id::at(i);
      }
    }
    if (count_ >= kMaxChannels) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }
    numbers_[count_] = wire_channel;
    ++count_;
    return channel_id::at(count_ - 1);
  }

  // Looks up a channel number seen on the wire.
  //
  // An unregistered channel is `not_supported` rather than being added on demand.
  // Growing the table from wire data would let a corrupted channel field silently
  // consume configuration slots, and(worse) a receiver would start tracking
  // sequence numbers for a stream nobody asked it to follow, reporting gaps in it.
  [[nodiscard]] constexpr result<channel_id> find(
      std::uint32_t wire_channel) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (numbers_[i] == wire_channel) {
        return channel_id::at(i);
      }
    }
    return error::not_supported;
  }

  [[nodiscard]] constexpr std::uint32_t wire_channel_of(
      channel_id id) const noexcept {
    DFR_ASSERT(id.index() < count_, "channel id was never registered");
    return numbers_[id.index()];
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

 private:
  std::array<std::uint32_t, kMaxChannels> numbers_{};
  std::size_t count_{0};
};

}  // namespace recovery
}  // namespace dfr::inline v1

#endif  // DFR_RECOVERY_CHANNEL_HPP

// The retransmit server, with a retention window that really does forget.
//
// A ring buffer here, a linear buffer there, and the difference is the point
// -----------------------------------------------------------------------
// recovery::replay_buffer refuses when it fills, because silently dropping its oldest entries
// is what makes the Glimpse race undetectable. This does the opposite and overwrites its
// oldest entries, because that is not a compromise: it is the definition of a retention
// window. A retransmit facility that never forgot would be a facility no client could ever
// provoke into saying no, and `error::retransmit_window_exceeded` would be dead code that a
// client had never been driven through.
//
// Being able to say no is most of the value
// ----------------------------------------
// Until now the only retransmit server in this project was a test harness that always had the
// answer. A client tested only against that is a client whose window-exceeded path has never
// run. Here the window is a configured number of packets, the oldest fall out of it, and a
// request that reaches back too far is refused the way a real facility refuses it.

#ifndef DFR_VENUE_RETRANSMIT_FACILITY_HPP
#define DFR_VENUE_RETRANSMIT_FACILITY_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/recovery/gap.hpp>
#include <dfr/venue/publisher.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dfr::inline v1::venue {

// MoldUDP64 caps a request at 60,000 messages, and a real server answers a larger one with
// silence. Modelled as a refusal rather than as silence so a test can tell the difference
// between the facility declining and the harness forgetting to ask: silence is indistinguishable
// from a bug in the test.
inline constexpr std::uint64_t kMaxMessagesPerResponse = 60'000;

struct facility_stats {
  std::uint64_t requests{0};
  std::uint64_t packets_served{0};
  std::uint64_t messages_served{0};
  std::uint64_t refused_too_old{0};
  std::uint64_t refused_too_large{0};
  // Requests for sequences the publisher has not produced yet: not an error and not servable,
  // and counted separately because it means the *client* is confused rather than late.
  std::uint64_t requests_ahead_of_feed{0};

  [[nodiscard]] friend constexpr bool operator==(const facility_stats&,
                                                 const facility_stats&) = default;
};

// `Packets` is the retention window, measured in datagrams rather than in time. A venue's
// published window is usually a duration, but a facility bounded by memory is bounded in
// packets, and converting one to the other requires a rate nobody controls, so the honest
// parameter is the one that is actually fixed.
template <std::size_t Packets = 256, std::size_t MaxDatagram = kMaxDatagramBytes>
class retransmit_facility {
 public:
  static_assert(Packets > 0, "a facility with no retention could never serve anything");

  constexpr retransmit_facility() noexcept = default;

  [[nodiscard]] constexpr const facility_stats& stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] constexpr std::size_t retained() const noexcept { return count_; }

  // How many packets have fallen out of the window. The number that says the retention limit
  // is doing something, which a test needs in order to know it is testing the refusal path
  // rather than a window that happened to be large enough.
  [[nodiscard]] constexpr std::uint64_t evicted() const noexcept { return evicted_; }
  [[nodiscard]] static constexpr std::size_t window() noexcept { return Packets; }

  // The messages still available. Empty until something has been published.
  [[nodiscard]] constexpr recovery::sequence_range available() const noexcept {
    if (count_ == 0) {
      return recovery::sequence_range{};
    }
    return recovery::sequence_range{.first = oldest().first_sequence,
                                    .end = newest().end_sequence()};
  }

  // Records a published packet, evicting the oldest when the window is full.
  //
  // Heartbeats are not retained: they carry no messages, so no request can ever be answered
  // with one, and keeping them would let a quiet period push real data out of the window.
  [[nodiscard]] constexpr result<void> record(std::uint64_t first_sequence,
                                              std::uint64_t message_count,
                                              packet_view packet) noexcept {
    if (message_count == 0) {
      return ok();
    }
    if (packet.size() > MaxDatagram) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    slot& into = slots_[next_];
    std::memcpy(into.bytes.data(), packet.data(), packet.size());
    into.size = packet.size();
    into.first_sequence = first_sequence;
    into.message_count = message_count;

    next_ = (next_ + 1) % Packets;
    if (count_ < Packets) {
      ++count_;
    } else {
      ++evicted_;
    }
    return ok();
  }

  // Answers a request, handing each covering packet to `emit`.
  //
  // Returns the range actually served, which is not always the range asked for: a request may
  // reach past the newest packet published, and serving its available prefix is what a real
  // facility does rather than refusing outright.
  template <typename Emit>
  [[nodiscard]] constexpr result<recovery::sequence_range> serve(
      recovery::sequence_range wanted, Emit&& emit) noexcept {
    ++stats_.requests;

    if (wanted.empty()) {
      return recovery::sequence_range{};
    }
    if (wanted.count() > kMaxMessagesPerResponse) DFR_UNLIKELY {
      ++stats_.refused_too_large;
      return error::capacity_exceeded;
    }
    if (count_ == 0) {
      ++stats_.requests_ahead_of_feed;
      return recovery::sequence_range{};
    }

    const recovery::sequence_range have = available();
    if (wanted.first < have.first) DFR_UNLIKELY {
      // The oldest message asked for has fallen out of the window. Refused whole rather than
      // partially served: a client handed the tail would close part of its hole and keep
      // asking for the rest forever, never learning that a snapshot is the only repair.
      ++stats_.refused_too_old;
      return error::retransmit_window_exceeded;
    }
    if (wanted.first >= have.end) {
      ++stats_.requests_ahead_of_feed;
      return recovery::sequence_range{};
    }

    recovery::sequence_range served{};
    for (std::size_t i = 0; i < count_; ++i) {
      const slot& held = at(i);
      const recovery::sequence_range carried{.first = held.first_sequence,
                                             .end = held.end_sequence()};
      if (!carried.overlaps(wanted)) {
        continue;
      }
      ++stats_.packets_served;
      stats_.messages_served += held.message_count;
      served = served.empty() ? carried : merge(served, carried);
      emit(packet_view{held.bytes.data(), held.size});
    }
    return served;
  }

 private:
  struct slot {
    std::array<std::byte, MaxDatagram> bytes{};
    std::size_t size{0};
    std::uint64_t first_sequence{0};
    std::uint64_t message_count{0};

    [[nodiscard]] constexpr std::uint64_t end_sequence() const noexcept {
      return first_sequence + message_count;
    }
  };

  // Index 0 is the oldest retained packet, so iteration is in publication order and a served
  // range comes out contiguous without a sort.
  [[nodiscard]] constexpr const slot& at(std::size_t index) const noexcept {
    DFR_ASSERT(index < count_, "reading past the retained packets");
    const std::size_t first = count_ < Packets ? 0 : next_;
    return slots_[(first + index) % Packets];
  }

  [[nodiscard]] constexpr const slot& oldest() const noexcept { return at(0); }
  [[nodiscard]] constexpr const slot& newest() const noexcept {
    return at(count_ - 1);
  }

  std::array<slot, Packets> slots_{};
  std::size_t next_{0};
  std::size_t count_{0};
  std::uint64_t evicted_{0};
  facility_stats stats_{};
};

}  // namespace dfr::inline v1::venue
#endif  // DFR_VENUE_RETRANSMIT_FACILITY_HPP

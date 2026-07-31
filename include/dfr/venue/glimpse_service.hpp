// A Glimpse service: a book, served as a SoupBinTCP session.
//
// `snapshot_facility` models when a snapshot is stale. This models what a snapshot *is*: a login, the state as
// ordinary feed messages, and an End Of Snapshot carrying the sequence the state is valid as of.
//
// Why serve a book rather than replay messages
// -------------------------------------------
// The obvious implementation of a snapshot is "resend every message from the start of the day", and it is wrong
// for the reason snapshots exist: a client asks for one because it *cannot* catch up by replay. So this walks the
// current book and emits one Price Level Update per level — which is O(depth), not O(messages), and is what makes
// a snapshot cheap enough to serve to a client that has fallen behind.
//
// The consequence is worth stating: **a snapshot carries state, not history.** A client that applies one has the
// right book and has seen none of the trades that built it. Anything reconciling volume has to know that, and a
// service that pretended otherwise by replaying trades would be replaying the day.
//
// The race is still here, and still the point
// ------------------------------------------
// The position is captured when the request arrives, not when the reply is sent. Everything published in between
// is *not* in the snapshot and *is* above its sequence — which is exactly the gap a client must fill from its own
// buffer, and exactly the gap it cannot fill if the buffer did not reach back far enough. That is the Glimpse
// race, and serving over a real protocol does not soften it.

#ifndef DFR_VENUE_GLIMPSE_SERVICE_HPP
#define DFR_VENUE_GLIMPSE_SERVICE_HPP

#include <dfr/book/order_book.hpp>
#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/deep/encode.hpp>
#include <dfr/wire/glimpse.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1 {
namespace venue {

struct glimpse_stats {
  std::uint64_t sessions_served{0};
  std::uint64_t levels_sent{0};
  std::uint64_t packets_sent{0};

  [[nodiscard]] friend constexpr bool operator==(const glimpse_stats&,
                                                 const glimpse_stats&) = default;
};

class glimpse_service {
 public:
  explicit constexpr glimpse_service(std::uint32_t session,
                                     std::string_view session_id = "DFRGLIMPSE") noexcept
      : session_(session), session_id_(session_id) {
    DFR_ASSERT(session_id.size() <= wire::soupbintcp::kSessionSize,
               "a Glimpse session identifier must fit the Login Accepted field");
  }

  [[nodiscard]] constexpr const glimpse_stats& stats() const noexcept { return stats_; }

  /**
   * Serves one whole snapshot session for one symbol's book.
   *
   * `next_sequence` is the position captured **when the request arrived**, not now. The caller passes it in
   * rather than the service reading a clock, for the same reason nothing else here reads one: the race this
   * models has to be arrangeable by a test rather than dependent on how long a machine took.
   *
   * Emits complete SoupBinTCP frames. The caller sends them; this opens no socket.
   */
  template <std::size_t Depth, typename Emit>
  [[nodiscard]] result<void> serve(const book::order_book<Depth>& state, std::string_view symbol,
                                   std::uint64_t next_sequence, Emit&& emit) noexcept {
    if (next_sequence == 0) DFR_UNLIKELY {
      // A snapshot valid as of nothing is not a snapshot, and a client resuming from zero would replay the day.
      return error::invalid_argument;
    }
    ++stats_.sessions_served;

    if (const auto err = send_login(emit); !err) DFR_UNLIKELY {
      return err;
    }

    // Begin, so a client that connected to the wrong service finds out before applying state.
    std::array<std::byte, wire::glimpse::kMessageSize> marker{};
    std::size_t marker_size = 0;
    if (const auto err =
            wire::glimpse::encode_begin(mutable_packet_view{marker.data(), marker.size()}, session_)
                .get(marker_size);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    if (const auto err = send_sequenced(packet_view{marker.data(), marker_size}, emit);
        !err) DFR_UNLIKELY {
      return err;
    }

    // The book, bids then asks, best price first on each side. Order within a side matters to nobody applying
    // it — a book is a set of levels — but emitting them in book order means a reader watching the stream sees
    // the same shape they will end up with.
    for (const auto& level : state.bids().levels()) {
      if (const auto err = send_level(symbol, /*buy=*/true, level, emit); !err) DFR_UNLIKELY {
        return err;
      }
    }
    for (const auto& level : state.asks().levels()) {
      if (const auto err = send_level(symbol, /*buy=*/false, level, emit); !err) DFR_UNLIKELY {
        return err;
      }
    }

    if (const auto err = wire::glimpse::encode_end(
                             mutable_packet_view{marker.data(), marker.size()}, session_,
                             next_sequence)
                             .get(marker_size);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    if (const auto err = send_sequenced(packet_view{marker.data(), marker_size}, emit);
        !err) DFR_UNLIKELY {
      return err;
    }

    // End Of Session, so a client knows the snapshot is complete rather than truncated by a dropped connection.
    // Without it, a snapshot that ended early and one that ended are the same thing on the wire.
    return send_bare(wire::soupbintcp::packet_type::end_of_session, emit);
  }

 private:
  static constexpr std::size_t kFrameCapacity =
      wire::soupbintcp::kFrameOverhead + wire::soupbintcp::kMaxPacketBytes;

  template <typename Emit>
  [[nodiscard]] result<void> send_login(Emit&& emit) noexcept {
    std::array<std::byte, 64> out{};
    std::size_t size = 0;
    // The sequence in Login Accepted is the *snapshot stream's* first packet, which is one: a Glimpse session is
    // its own numbered stream and has nothing to do with the live feed's numbering. Conflating the two is a
    // tempting mistake, because both are sequence numbers on the same kind of packet.
    if (const auto err = wire::soupbintcp::encode_login_accepted(
                             mutable_packet_view{out.data(), out.size()}, session_id_, 1)
                             .get(size);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    ++stats_.packets_sent;
    emit(packet_view{out.data(), size});
    return ok();
  }

  template <typename Emit>
  [[nodiscard]] result<void> send_level(std::string_view symbol, bool buy, book::level level,
                                        Emit&& emit) noexcept {
    std::array<std::byte, wire::deep::kSymbolOffset + 64> body{};
    std::size_t body_size = 0;
    // Timestamp zero: a snapshot level has no moment of its own, and inventing one would put a time on the wire
    // that no event happened at. A consumer that needs the instant has the End Of Snapshot's sequence.
    if (const auto err = wire::deep::encode_price_level(
                             mutable_packet_view{body.data(), body.size()}, buy, symbol, level.size,
                             level.at, /*timestamp_ns=*/0)
                             .get(body_size);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    ++stats_.levels_sent;
    return send_sequenced(packet_view{body.data(), body_size}, emit);
  }

  template <typename Emit>
  [[nodiscard]] result<void> send_sequenced(packet_view body, Emit&& emit) noexcept {
    std::array<std::byte, 512> out{};
    std::size_t size = 0;
    if (const auto err = wire::soupbintcp::encode_packet(
                             mutable_packet_view{out.data(), out.size()},
                             wire::soupbintcp::packet_type::sequenced_data, body)
                             .get(size);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    ++stats_.packets_sent;
    emit(packet_view{out.data(), size});
    return ok();
  }

  template <typename Emit>
  [[nodiscard]] result<void> send_bare(wire::soupbintcp::packet_type type, Emit&& emit) noexcept {
    std::array<std::byte, wire::soupbintcp::kFrameOverhead> out{};
    std::size_t size = 0;
    if (const auto err =
            wire::soupbintcp::encode_bare(mutable_packet_view{out.data(), out.size()}, type)
                .get(size);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    ++stats_.packets_sent;
    emit(packet_view{out.data(), size});
    return ok();
  }

  std::uint32_t session_{0};
  std::string_view session_id_{};
  glimpse_stats stats_{};
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_GLIMPSE_SERVICE_HPP

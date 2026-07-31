// Publishing a market-data feed the way a venue does.
//
// Until now every test stream was built by a test helper. That is fine for checking a decoder
// and useless for checking a receiver, because a helper produces the packets the test author
// thought of. A publisher produces the packets a *venue* produces: messages packed until the
// datagram is full, heartbeats during quiet periods, and both of IEX-TP's redundant chains
// maintained exactly.
//
// The two chains are the reason this is worth building rather than faking
// ---------------------------------------------------------------------
// Sequence numbers count messages; Stream Offset counts payload bytes. A publisher that got
// either wrong would produce a stream `chain_checker` rejects — and `chain_checker` is the
// component that held on 460,578 real IEX packets. So the encoder and the decoder check each
// other here, and the decoder has already been checked against reality. That is a stronger
// position than either alone.
//
// IEX-TP only, for now, and deliberately not generalised
// ----------------------------------------------------
// A MoldUDP64 publisher is the same shape with a different encoder and no stream offset. The
// injector is parameterised over a protocol because two implementations existed to factor;
// here there is one, and building the policy first would be designing for a case not yet
// written. dfr::chaos::fault_target is what that abstraction should look like when the second
// publisher lands.

#ifndef DFR_VENUE_PUBLISHER_HPP
#define DFR_VENUE_PUBLISHER_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/iextp/encode.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace venue {

// A datagram this size fits inside a 1500-byte Ethernet MTU with room for the IPv4 and UDP
// headers and a VLAN tag. Chosen rather than 1500 because a publisher that emitted packets
// which fragmented in transit would be testing IP reassembly, not feed recovery — and the
// 2017 IEX capture carries a VLAN tag while later ones do not, so the allowance has to hold
// either way.
inline constexpr std::size_t kMaxDatagramBytes = 1400;

struct publisher_options {
  std::uint32_t session{1};
  std::uint32_t channel{1};

  // Where the feed's numbering starts. Not necessarily one: a receiver joining mid-session is
  // the normal case, and a publisher that always started at one would let a client get away
  // with assuming it.
  std::uint64_t first_sequence{1};
  std::int64_t first_stream_offset{0};

  // How long a quiet period may last before a heartbeat goes out. IEX sends one about once a
  // second; the value matters because the heartbeat is how a receiver learns it missed the end
  // of a quiet period.
  duration heartbeat_interval{std::chrono::seconds{1}};

  [[nodiscard]] constexpr result<void> validate() const noexcept {
    if (heartbeat_interval <= duration::zero()) {
      return error::invalid_argument;
    }
    return ok();
  }
};

struct publisher_stats {
  std::uint64_t packets{0};
  std::uint64_t messages{0};
  std::uint64_t heartbeats{0};
  std::uint64_t bytes{0};

  [[nodiscard]] friend constexpr bool operator==(const publisher_stats&,
                                                 const publisher_stats&) = default;
};

template <clock_source Clock, std::size_t MaxDatagram = kMaxDatagramBytes>
class iextp_publisher {
 public:
  using time_point = typename Clock::time_point;

  explicit constexpr iextp_publisher(publisher_options options) noexcept
      : options_(options),
        next_sequence_(options.first_sequence),
        next_offset_(options.first_stream_offset) {
    DFR_ASSERT(options_.validate().has_value(),
               "publisher options must be validated before use");
  }

  [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept {
    return next_sequence_;
  }
  [[nodiscard]] constexpr std::int64_t next_stream_offset() const noexcept {
    return next_offset_;
  }
  [[nodiscard]] constexpr const publisher_stats& stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] constexpr bool has_pending() const noexcept { return open_; }
  [[nodiscard]] constexpr std::uint16_t pending_messages() const noexcept {
    return open_ ? builder_.message_count() : 0;
  }

  // Adds one message to the feed, emitting a packet when the datagram is full.
  //
  // Packing rather than one message per packet, because that is what a venue does and because
  // a receiver tested only against one-message packets never exercises the distinction
  // between a lost packet and a lost message — which is the distinction the whole of
  // dfr::recovery is built on.
  template <typename Emit>
  [[nodiscard]] constexpr result<void> submit(packet_view message, time_point now,
                                              Emit&& emit) noexcept {
    if (message.size() + wire::iextp::kMessageLengthSize +
            wire::iextp::kHeaderSize >
        MaxDatagram) DFR_UNLIKELY {
      // No packet could ever carry it, so this is a caller error rather than a full buffer.
      // Reported rather than asserted because the message came from outside.
      return error::invalid_argument;
    }

    if (!open_) {
      if (const auto err = open(); !err) DFR_UNLIKELY {
        return err;
      }
    }
    if (const auto err = builder_.append(message); !err) {
      // Full. Send what is there and start a fresh datagram — never split a message across
      // two packets, which IEX-TP has no way to express.
      if (const auto flushed = flush(now, emit); !flushed) DFR_UNLIKELY {
        return flushed;
      }
      if (const auto reopened = open(); !reopened) DFR_UNLIKELY {
        return reopened;
      }
      if (const auto retried = builder_.append(message); !retried) DFR_UNLIKELY {
        return retried;
      }
    }
    return ok();
  }

  // Sends whatever is pending, if anything.
  //
  // Emitting nothing when nothing is pending rather than an empty packet: an empty data packet
  // and a heartbeat are the same bytes on the wire, and conflating them would make the
  // heartbeat count meaningless.
  template <typename Emit>
  [[nodiscard]] constexpr result<void> flush(time_point now, Emit&& emit) noexcept {
    if (!open_ || builder_.message_count() == 0) {
      open_ = false;
      return ok();
    }

    packet_view finished;
    if (const auto err = builder_.finish().get(finished); err != error::ok)
        DFR_UNLIKELY {
      return err;
    }
    open_ = false;
    advance(finished, /*is_heartbeat=*/false);
    last_emit_ = now;
    emit(finished);
    return ok();
  }

  // Emits a heartbeat if the feed has been quiet for long enough.
  //
  // Poll-driven like everything else: the publisher never reads a clock, so a venue runs at
  // whatever rate its driver chooses and a replay is reproducible from the same call sequence.
  template <typename Emit>
  [[nodiscard]] constexpr result<void> poll(time_point now, Emit&& emit) noexcept {
    if (open_ && builder_.message_count() > 0) {
      return flush(now, emit);
    }
    if (started_ && now - last_emit_ < options_.heartbeat_interval) {
      return ok();
    }
    return send_heartbeat(now, emit);
  }

  // Sends a heartbeat regardless of the interval, for a test that wants one exactly here.
  template <typename Emit>
  [[nodiscard]] constexpr result<void> send_heartbeat(time_point now,
                                                      Emit&& emit) noexcept {
    DFR_ASSERT(!open_ || builder_.message_count() == 0,
               "a heartbeat must not overtake pending messages");
    open_ = false;

    const wire::iextp::header prototype{.channel = options_.channel,
                                        .session = options_.session,
                                        .stream_offset = next_offset_,
                                        .first_sequence = next_sequence_};
    std::size_t written = 0;
    if (const auto err =
            wire::iextp::encode_heartbeat(mutable_packet_view{buffer_.data(),
                                                              buffer_.size()},
                                          prototype)
                .get(written);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }

    const packet_view beat{buffer_.data(), written};
    // A heartbeat carries the sequence of the *next* message and the current offset, and
    // advances neither. That is what makes it able to announce a gap without delivering
    // anything.
    ++stats_.packets;
    ++stats_.heartbeats;
    stats_.bytes += written;
    started_ = true;
    last_emit_ = now;
    emit(beat);
    return ok();
  }

 private:
  [[nodiscard]] constexpr result<void> open() noexcept {
    const wire::iextp::header prototype{.channel = options_.channel,
                                        .session = options_.session,
                                        .stream_offset = next_offset_,
                                        .first_sequence = next_sequence_};
    if (const auto err = wire::iextp::packet_builder::into(
                             mutable_packet_view{buffer_.data(), buffer_.size()},
                             prototype)
                             .get(builder_);
        err != error::ok) DFR_UNLIKELY {
      return err;
    }
    open_ = true;
    return ok();
  }

  // Both chains advance here and nowhere else, so there is one place to be wrong and one place
  // to check: sequence numbers by the message count, stream offset by the payload byte count.
  constexpr void advance(packet_view finished, bool is_heartbeat) noexcept {
    const std::size_t payload = finished.size() - wire::iextp::kHeaderSize;
    if (!is_heartbeat) {
      next_sequence_ += builder_.message_count();
      next_offset_ += static_cast<std::int64_t>(payload);
      stats_.messages += builder_.message_count();
    }
    ++stats_.packets;
    stats_.bytes += finished.size();
    started_ = true;
  }

  publisher_options options_{};
  std::array<std::byte, MaxDatagram> buffer_{};
  wire::iextp::packet_builder builder_{};
  bool open_{false};
  bool started_{false};
  time_point last_emit_{};
  std::uint64_t next_sequence_{0};
  std::int64_t next_offset_{0};
  publisher_stats stats_{};
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_PUBLISHER_HPP

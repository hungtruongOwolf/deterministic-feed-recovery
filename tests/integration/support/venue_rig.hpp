// A whole venue and a whole client, wired together.
//
// Publisher → injector → client, with a retransmit facility and a snapshot facility answering
// on the side. Nothing here is a stub: the facility has a retention window that really forgets
// and refuses when a request reaches past it, and the snapshot facility captures its position at
// request time so it can lose the Glimpse race.
//
// The clock is explicit and shared, because a venue and a client that disagreed about the time
// would produce timelines no test could reason about.

#ifndef DFR_TESTS_INTEGRATION_SUPPORT_VENUE_RIG_HPP
#define DFR_TESTS_INTEGRATION_SUPPORT_VENUE_RIG_HPP

#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/schedule.hpp>
#include <dfr/chaos/target.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/venue/publisher.hpp>
#include <dfr/venue/retransmit_facility.hpp>
#include <dfr/venue/snapshot_facility.hpp>
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dfr_test::venue_rig {

namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;
namespace rec = dfr::recovery;
namespace ven = dfr::venue;

inline constexpr std::uint32_t kSession = 0xBEEF;

using rig_clock = dfr::manual_clock;
using rig_time = rig_clock::time_point;
using rig_publisher = ven::iextp_publisher<rig_clock>;
using rig_client = rec::client<rig_clock, rec::replay_buffer<8192, 1024>>;

inline rig_time at_us(std::int64_t micros) {
  return rig_time{} + std::chrono::microseconds{micros};
}

// What crossed the boundary, and what the venue was asked for.
struct rig_ledger {
  std::map<std::uint64_t, int> deliveries;
  std::set<std::uint64_t> offered;
  std::uint64_t discarded{0};
  std::uint64_t retransmit_requests{0};
  std::uint64_t retransmit_refusals{0};
  std::uint64_t snapshot_requests{0};
  std::uint64_t snapshots_applied{0};
  std::uint64_t replays_finished{0};

  [[nodiscard]] std::vector<std::uint64_t> delivered_twice() const {
    std::vector<std::uint64_t> out;
    for (const auto& [sequence, count] : deliveries) {
      if (count > 1) {
        out.push_back(sequence);
      }
    }
    return out;
  }
};

inline rec::client_options rig_client_options() {
  rec::client_options options;
  options.lines = 1;
  options.arbitration.detect_divergence = false;
  options.arbitration.liveness_timeout = std::chrono::seconds{10};
  options.retransmission.settle_delay = std::chrono::microseconds{1};
  options.retransmission.first_timeout = std::chrono::microseconds{10};
  options.retransmission.max_timeout = std::chrono::microseconds{50};
  options.retransmission.max_attempts = 3;
  options.retransmission.retention_window = std::chrono::seconds{5};
  REQUIRE(options.validate().has_value());
  return options;
}

inline ven::publisher_options rig_publisher_options() {
  ven::publisher_options options;
  options.session = kSession;
  options.channel = 1;
  options.first_sequence = 1;
  options.heartbeat_interval = std::chrono::milliseconds{500};
  REQUIRE(options.validate().has_value());
  return options;
}

// Hands a packet to the client if it survives decoding, recording what happened. A packet that
// fails to decode or frame is discarded exactly as a real receiver discards it.
inline void offer(rig_client& client, rig_ledger& record, dfr::packet_view packet,
                  rig_time now) {
  iex::header header;
  if (iex::decode_header(packet).get(header) != dfr::error::ok) {
    ++record.discarded;
    return;
  }
  if (!iex::verify_payload_framing(packet)) {
    ++record.discarded;
    return;
  }

  rec::ingest_report report;
  const auto outcome = client.on_packet(0, header.session, header.first_sequence,
                                        header.message_count, 0, now);
  if (outcome.get(report) != dfr::error::ok) {
    return;  // the client has said why; the caller reads its state
  }
  for (std::uint64_t s = header.first_sequence;
       s < header.first_sequence + header.message_count; ++s) {
    record.offered.insert(s);
  }

  if (report.held_for_replay) {
    // The client owes nothing here; the caller owes it the messages, one at a time, because
    // splitting a packet needs the wire cursor. Payload bytes are irrelevant to the accounting,
    // so a single byte per message keeps the buffer small and the test about sequencing.
    for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
      const char body = 'm';
      if (!client.buffer_message(s, dfr::packet_view{&body, 1})) {
        return;  // buffer full or holed: the client is now failed and says so
      }
    }
    return;
  }
  if (!report.delivered()) {
    return;
  }
  for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
    ++record.deliveries[s];
  }
  for (const auto& repaired : client.last_recovered().ranges()) {
    for (std::uint64_t s = repaired.first; s < repaired.end; ++s) {
      ++record.deliveries[s];
    }
  }
}

// A published packet, kept so the injector has a source stream and the facilities a record.
struct published {
  std::string bytes;
  std::uint64_t first_sequence{0};
  std::uint64_t message_count{0};
};

// One packet's worth of message sequences.
inline rec::sequence_range carried_by(const published& packet) {
  return rec::sequence_range{.first = packet.first_sequence,
                             .end = packet.first_sequence + packet.message_count};
}

// Runs the venue for `messages` messages and returns everything it published, in order.
inline std::vector<published> publish(rig_publisher& publisher, std::size_t messages) {
  std::vector<published> out;
  const auto capture = [&](dfr::packet_view packet) {
    iex::header header;
    REQUIRE(iex::decode_header(packet).get(header) ==
            dfr::error::ok);
    out.push_back(published{
        .bytes = std::string{reinterpret_cast<const char*>(packet.data()),
                             packet.size()},
        .first_sequence = header.first_sequence,
        .message_count = header.message_count});
  };

  std::int64_t now = 0;
  for (std::size_t i = 0; i < messages; ++i) {
    const std::string body = "msg-" + std::to_string(i);
    now += 5;
    REQUIRE(publisher
                .submit(dfr::packet_view{body.data(), body.size()}, at_us(now),
                        capture)
                .has_value());
    if (i % 3 == 2) {
      REQUIRE(publisher.flush(at_us(now), capture).has_value());
    }
  }
  REQUIRE(publisher.flush(at_us(now), capture).has_value());
  return out;
}


}  // namespace dfr_test::venue_rig

#endif  // DFR_TESTS_INTEGRATION_SUPPORT_VENUE_RIG_HPP

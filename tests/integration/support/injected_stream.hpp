// A clean IEX-TP stream, a fault schedule over it, and the ground truth to check against.
//
// The harness deliberately derives nothing from the fault vocabulary. It records what it
// actually managed to decode and hand to the client, and the oracle compares the client's
// accounting against that. A harness that computed "a drop of packet 12 should produce a
// hole of 3 messages" would be reimplementing the injector, and the two would agree by
// construction rather than by being right.

#ifndef DFR_TESTS_INTEGRATION_SUPPORT_INJECTED_STREAM_HPP
#define DFR_TESTS_INTEGRATION_SUPPORT_INJECTED_STREAM_HPP

#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/schedule.hpp>
#include <dfr/chaos/target.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/header.hpp>

#include "wire/support/raw_iextp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dfr_test::integration {

namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;
namespace rec = dfr::recovery;

inline constexpr std::uint32_t kSession = 0x5EED;
inline constexpr std::uint32_t kChannel = 1;

struct source_packet {
  std::string bytes;
  std::uint64_t first_sequence{0};
  std::uint64_t message_count{0};
};

// A truthfully chained stream: sequence numbers and stream offsets both continuous, which
// is what the IEX HIST corpus was verified to be across 460,578 real packets.
inline std::vector<source_packet> clean_stream(std::size_t packets) {
  std::vector<source_packet> out;
  out.reserve(packets);
  std::uint64_t sequence = 1;
  std::int64_t offset = 0;

  for (std::size_t i = 0; i < packets; ++i) {
    // One to three messages per packet, so a hole is not always the same size and the
    // message-versus-packet distinction is exercised rather than assumed away.
    //
    // Every fourth packet is a heartbeat, carrying no messages at all. Not decoration: two
    // thirds of a real IEX capture is heartbeats, and a stream without them let a defect
    // through that only appeared when the tool was pointed at real data — a heartbeat
    // advances the tracker's expectation without advancing the arbiter's watermark, and a
    // retransmit filling the resulting hole was then counted twice.
    const std::uint64_t count = (i % 4 == 3) ? 0 : (i % 3) + 1;
    dfr_test::iex::raw_packet packet;
    packet.session(kSession)
        .channel(kChannel)
        .first_sequence(sequence)
        .stream_offset(offset)
        .count(static_cast<std::uint16_t>(count));

    std::int64_t added = 0;
    for (std::uint64_t m = 0; m < count; ++m) {
      const std::string payload = "m" + std::to_string(sequence + m);
      packet.block(payload);
      added += static_cast<std::int64_t>(2 + payload.size());
    }
    packet.seal();

    const auto view = packet.view();
    out.push_back(source_packet{
        .bytes = std::string{reinterpret_cast<const char*>(view.data()),
                             view.size()},
        .first_sequence = sequence,
        .message_count = count});
    sequence += count;
    offset += added;
  }
  return out;
}

// The fault kinds whose consequence the harness can derive without reimplementing the
// decoder: the packet either does not arrive, or arrives too damaged to frame, or arrives
// twice or late.
//
// Excluded deliberately, each with its own targeted test instead:
//   flip_bit        — a flipped bit can land in a header field and silently redefine which
//                     messages the packet claims to carry, so "what should have happened"
//                     is not derivable from the fault alone.
//   rewrite_sequence, rewrite_session — these redefine the stream's position or identity,
//                     so an accounting identity across them is a category error rather
//                     than a property.
inline chaos::op_mask derivable_faults() {
  chaos::op_mask mask;
  mask.disable(chaos::fault_op::flip_bit);
  mask.disable(chaos::fault_op::rewrite_sequence);
  mask.disable(chaos::fault_op::rewrite_session);
  return mask;
}

// What actually happened, recorded rather than predicted.
struct ledger {
  // How many times each message sequence was handed downstream. Anything other than one,
  // for a sequence inside the observed span, is a defect.
  std::map<std::uint64_t, int> deliveries;

  // Sequences carried by a packet the harness successfully decoded and offered.
  std::set<std::uint64_t> offered;

  std::uint64_t packets_offered{0};
  std::uint64_t packets_discarded{0};   // failed to decode or to frame
  std::uint64_t retransmits_served{0};
  bool client_left_live{false};

  [[nodiscard]] std::uint64_t delivered_once() const {
    std::uint64_t total = 0;
    for (const auto& [sequence, count] : deliveries) {
      if (count == 1) {
        ++total;
      }
    }
    return total;
  }

  [[nodiscard]] std::vector<std::uint64_t> delivered_more_than_once() const {
    std::vector<std::uint64_t> out;
    for (const auto& [sequence, count] : deliveries) {
      if (count > 1) {
        out.push_back(sequence);
      }
    }
    return out;
  }
};

// A client tuned so a test reads as a timeline: it asks almost immediately and gives up
// quickly, because a test that waited a realistic 30 seconds of retention would be a test
// nobody runs.
inline rec::client_options oracle_options() {
  rec::client_options options;
  options.lines = 1;
  // One line cannot disagree with itself, so the digest comparison has nothing to do here.
  // Divergence has its own tests, where two lines exist.
  options.arbitration.detect_divergence = false;
  options.arbitration.liveness_timeout = std::chrono::seconds{10};
  options.retransmission.settle_delay = std::chrono::microseconds{1};
  options.retransmission.first_timeout = std::chrono::microseconds{10};
  options.retransmission.max_timeout = std::chrono::microseconds{100};
  options.retransmission.max_attempts = 5;
  options.retransmission.retention_window = std::chrono::seconds{5};
  REQUIRE(options.validate().has_value());
  return options;
}

}  // namespace dfr_test::integration

#endif  // DFR_TESTS_INTEGRATION_SUPPORT_INJECTED_STREAM_HPP

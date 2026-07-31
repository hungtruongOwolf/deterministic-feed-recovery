// The pipeline the verify tool drives: a client, a retransmit server, and a tally.
//
// Split from verify.cpp so that file is argument parsing, the run, and the report — the
// three things a reader opens it for. docs/STYLE.md §1.10: one file, one concept, and the
// rule applies to tools.
//
// This deliberately mirrors tests/integration/support/oracle_harness.hpp rather than sharing
// with it. The test harness is built on synthetic packets and Catch2 assertions; this one is
// built on a real capture and an exit code. Forcing one abstraction over both would make each
// harder to read than the small amount of parallel structure costs.

#ifndef DFR_TOOLS_SUPPORT_VERIFY_PIPELINE_HPP
#define DFR_TOOLS_SUPPORT_VERIFY_PIPELINE_HPP

#include <dfr/core/clock.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/header.hpp>
#include <dfr/chaos/schedule.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dfr_tools {

namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;
namespace rec = dfr::recovery;

using verify_client = rec::client<dfr::manual_clock, rec::replay_buffer<1 << 16, 4096>>;
using verify_time = dfr::manual_clock::time_point;

inline verify_time at_us(std::int64_t micros) {
  return verify_time{} + std::chrono::microseconds{micros};
}

struct source_packet {
  std::string bytes;
  std::uint64_t first_sequence{0};
  std::uint64_t message_count{0};
};

struct tally {
  std::map<std::uint64_t, int> deliveries;
  std::set<std::uint64_t> offered;
  std::uint64_t offered_packets{0};
  std::uint64_t discarded_packets{0};
  std::uint64_t retransmits_served{0};
  std::uint64_t double_deliveries{0};
  bool left_live{false};
};

// The same policy shape the unit oracle uses: asks almost immediately and gives up quickly,
// because the tool advances a simulated clock rather than waiting on a real one.
inline rec::client_options verify_options() {
  rec::client_options options;
  options.lines = 1;
  options.arbitration.detect_divergence = false;
  options.arbitration.liveness_timeout = std::chrono::seconds{60};
  options.retransmission.settle_delay = std::chrono::microseconds{1};
  options.retransmission.first_timeout = std::chrono::microseconds{10};
  options.retransmission.max_timeout = std::chrono::microseconds{100};
  options.retransmission.max_attempts = 5;
  options.retransmission.retention_window = std::chrono::seconds{30};
  return options;
}

// The same exclusions the unit oracle makes, and for the same reason: a flipped bit can land
// in a header field and silently redefine which messages a packet claims to carry, so its
// expected consequence is not derivable without reimplementing the decoder.
inline chaos::op_mask derivable_faults() {
  chaos::op_mask mask;
  mask.disable(chaos::fault_op::flip_bit);
  mask.disable(chaos::fault_op::rewrite_sequence);
  mask.disable(chaos::fault_op::rewrite_session);
  return mask;
}

inline void offer_if_intact(verify_client& client, tally& record, dfr::packet_view packet,
                     verify_time now) {
  iex::header header;
  if (iex::decode_header(packet).get(header) != dfr::error::ok) {
    ++record.discarded_packets;
    return;
  }
  if (!iex::verify_payload_framing(packet)) {
    ++record.discarded_packets;
    return;
  }

  rec::ingest_report report;
  if (client
          .on_packet(0, header.session, header.first_sequence,
                     header.message_count, 0, now)
          .get(report) != dfr::error::ok) {
    record.left_live = true;
    return;
  }
  ++record.offered_packets;
  for (std::uint64_t s = header.first_sequence;
       s < header.first_sequence + header.message_count; ++s) {
    record.offered.insert(s);
  }
  if (!report.delivered()) {
    return;
  }
  for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
    if (++record.deliveries[s] == 2) {
      ++record.double_deliveries;
    }
  }
  for (const auto& repaired : client.last_recovered().ranges()) {
    for (std::uint64_t s = repaired.first; s < repaired.end; ++s) {
      if (++record.deliveries[s] == 2) {
        ++record.double_deliveries;
      }
    }
  }
}

// The retransmit server, backed by the undamaged capture.
//
// Indexes only the packets that actually carry messages, and that is not an optimisation.
// Two thirds of a real IEX capture is heartbeats, and a heartbeat repeats the sequence
// number of the next message — so several packets share a first sequence, and a map keyed on
// it silently keeps whichever arrived first. When that was a heartbeat, the data packet
// became unfindable and every retransmit request was answered with nothing. The tool then
// blamed the client for a gap it had asked about five times.
class retransmit_server {
 public:
  explicit retransmit_server(const std::vector<source_packet>& stream)
      : stream_(stream) {
    index_.reserve(stream_.size());
    for (std::size_t i = 0; i < stream_.size(); ++i) {
      if (stream_[i].message_count > 0) {
        index_.push_back(entry{.first_sequence = stream_[i].first_sequence,
                               .at = i});
      }
    }
    std::sort(index_.begin(), index_.end(),
              [](const entry& a, const entry& b) {
                return a.first_sequence < b.first_sequence;
              });
  }

  void serve(verify_client& client, tally& record, rec::sequence_range wanted,
             verify_time now) const {
    // Start one before the first packet at or after `wanted.first`: the packet containing
    // that sequence may begin earlier.
    auto it = std::lower_bound(index_.begin(), index_.end(), wanted.first,
                               [](const entry& e, std::uint64_t value) {
                                 return e.first_sequence < value;
                               });
    if (it != index_.begin()) {
      --it;
    }
    for (; it != index_.end(); ++it) {
      const source_packet& packet = stream_[it->at];
      if (packet.first_sequence >= wanted.end) {
        break;
      }
      const rec::sequence_range carried{
          .first = packet.first_sequence,
          .end = packet.first_sequence + packet.message_count};
      if (!carried.overlaps(wanted)) {
        continue;
      }
      ++record.retransmits_served;
      offer_if_intact(client, record,
                      dfr::packet_view{packet.bytes.data(), packet.bytes.size()},
                      now);
    }
  }

 private:
  struct entry {
    std::uint64_t first_sequence{0};
    std::size_t at{0};
  };

  const std::vector<source_packet>& stream_;
  std::vector<entry> index_;
};

inline void drain(verify_client& client, tally& record, const retransmit_server& server,
           std::int64_t now_us) {
  for (int step = 0; step < 64; ++step) {
    const auto decision = client.poll(at_us(now_us));
    if (decision.what == rec::client_action::idle) {
      return;
    }
    if (decision.what != rec::client_action::send_retransmit_request) {
      record.left_live = true;
      return;
    }
    server.serve(client, record, decision.range, at_us(now_us));
  }
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_VERIFY_PIPELINE_HPP

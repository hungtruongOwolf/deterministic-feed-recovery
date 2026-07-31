// One complete run of the whole library, recorded.
//
// Publisher → injector → wire → client, with a retransmit facility and a snapshot facility
// answering on the side, and a trace::recorder collecting what happened at every step. Every event
// is built from what the components already returned, so the trace cannot disagree with the run.
//
// Two modes, because the interesting failure has to be arranged
// -----------------------------------------------------------
// `recovering` is the ordinary run: faults are injected and the retransmit facility repairs them.
// `glimpse` refuses to serve retransmits at all, which drives the client to a snapshot, then loses
// packets while the snapshot is being built so that a stale reply lands in the gap between what the
// client delivered and the oldest thing it managed to buffer. That interval is the only place the
// race exists, and it does not occur on a contiguous stream — see docs/DESIGN.md and the
// integration tests.

#ifndef DFR_TOOLS_SUPPORT_TRACED_RUN_HPP
#define DFR_TOOLS_SUPPORT_TRACED_RUN_HPP

#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/schedule.hpp>
#include <dfr/chaos/target.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/trace/recorder.hpp>
#include <dfr/venue/publisher.hpp>
#include <dfr/venue/retransmit_facility.hpp>
#include <dfr/venue/snapshot_facility.hpp>
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dfr_tools {

namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;
namespace rec = dfr::recovery;
namespace ven = dfr::venue;
namespace trc = dfr::trace;

inline constexpr std::uint32_t kTraceSession = 0xBEEF;

using trace_clock = dfr::manual_clock;
using trace_time = trace_clock::time_point;
// Traces run to a few hundred events. The record grew when it learned to carry the outstanding holes,
// so the count comes down to match — 16,384 of the wider record would be a megabyte-scale stack member
// for headroom nothing uses.
using trace_recorder = trc::recorder<4'096>;
using trace_client = rec::client<trace_clock, rec::replay_buffer<16'384, 2048>>;

enum class run_mode { recovering, glimpse };

struct run_options {
  std::uint64_t seed{4711};
  std::size_t messages{300};
  std::uint32_t faults{6};
  std::uint64_t staleness_messages{0};

  // How many redundant lines carry the feed. Two is the interesting case: the venue publishes
  // once and each line loses a different set of packets, so most holes close from the other line
  // and never become retransmit requests at all.
  std::size_t lines{1};

  run_mode mode{run_mode::recovering};
};

struct traced_packet {
  std::string bytes;
  std::uint64_t first_sequence{0};
  std::uint64_t message_count{0};
};

// Everything a report needs that is not in the events themselves.
struct run_summary {
  std::size_t published_packets{0};
  std::uint64_t messages_delivered{0};
  std::uint64_t messages_delivered_twice{0};
  std::uint64_t messages_missing{0};
  std::uint64_t retransmit_requests{0};
  // How many *messages* those requests covered, as distinct from how many requests there were.
  //
  // Added because the two answer different questions and the difference turned out to be interesting. A
  // second line that fills the middle of a hole splits one gap into two, so it can produce *more* requests
  // while asking for *fewer messages* — seed 114 at 300 messages does exactly that: two requests covering 24
  // messages on two lines, one request covering 27 on one. The count is not monotone in the number of
  // defences and this is, which makes it the honest quantity to report.
  std::uint64_t retransmit_messages{0};
  std::uint64_t retransmits_served{0};
  std::uint64_t retransmit_refusals{0};
  std::uint64_t snapshot_requests{0};
  std::uint64_t unfillable_messages{0};
  rec::client_state final_state{rec::client_state::synchronising};
  std::vector<chaos::fault> schedule;
};

inline trace_time at_us(std::int64_t micros) {
  return trace_time{} + std::chrono::microseconds{micros};
}

inline rec::client_options trace_client_options(std::size_t lines) {
  rec::client_options options;
  // Told how many lines it is wired to, because live_lines() is a liveness summary over exactly
  // that count: a client configured for one line while two are feeding it would report a healthy
  // pair as a single line and hide the failure that matters.
  options.lines = lines;
  options.arbitration.detect_divergence = false;
  options.arbitration.liveness_timeout = std::chrono::seconds{10};
  options.retransmission.settle_delay = std::chrono::microseconds{1};
  options.retransmission.first_timeout = std::chrono::microseconds{10};
  options.retransmission.max_timeout = std::chrono::microseconds{80};
  options.retransmission.max_attempts = 3;
  options.retransmission.retention_window = std::chrono::seconds{5};
  return options;
}

inline ven::publisher_options trace_publisher_options() {
  ven::publisher_options options;
  options.session = kTraceSession;
  options.channel = 1;
  options.first_sequence = 1;
  options.heartbeat_interval = std::chrono::milliseconds{200};
  return options;
}

// The faults whose consequence is unambiguous: the packet does not arrive, arrives too damaged to
// frame, or arrives twice or late. flip_bit and the two rewrites are excluded for the reason the
// oracle excludes them — a flipped bit can silently redefine which messages a packet claims to
// carry.
inline chaos::op_mask traceable_faults() {
  chaos::op_mask mask;
  mask.disable(chaos::fault_op::flip_bit);
  mask.disable(chaos::fault_op::rewrite_sequence);
  mask.disable(chaos::fault_op::rewrite_session);
  return mask;
}

// Runs the venue and returns everything it published, in order.
inline std::vector<traced_packet> publish_stream(std::size_t messages,
                                                 trace_recorder& into,
                                                 std::int64_t& now_us) {
  ven::iextp_publisher<trace_clock> publisher{trace_publisher_options()};
  std::vector<traced_packet> out;

  const auto capture = [&](dfr::packet_view packet) {
    iex::header header;
    if (iex::decode_header(packet).get(header) != dfr::error::ok) {
      return;
    }
    out.push_back(traced_packet{
        .bytes = std::string{reinterpret_cast<const char*>(packet.data()),
                             packet.size()},
        .first_sequence = header.first_sequence,
        .message_count = header.message_count});

    const trc::context where{.packet_index = out.size() - 1,
                             .time_ns = now_us * 1'000};
    auto event = where.with(header.message_count == 0
                                ? trc::event_kind::heartbeat_sent
                                : trc::event_kind::published);
    event.first_sequence = header.first_sequence;
    event.end_sequence = header.first_sequence + header.message_count;
    event.detail = packet.size();
    (void)into.record(event);
  };

  for (std::size_t i = 0; i < messages; ++i) {
    const std::string body = "msg-" + std::to_string(i);
    now_us += 5;
    if (!publisher.submit(dfr::packet_view{body.data(), body.size()},
                          at_us(now_us), capture)) {
      break;
    }
    if (i % 3 == 2) {
      (void)publisher.flush(at_us(now_us), capture);
    }
    // A quiet period every so often, so heartbeats appear in the trace and the viewer shows the
    // mechanism by which a gap is announced without any data arriving.
    if (i % 47 == 46) {
      now_us += 250'000;
      (void)publisher.poll(at_us(now_us), capture);
    }
  }
  (void)publisher.flush(at_us(now_us), capture);
  return out;
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_TRACED_RUN_HPP

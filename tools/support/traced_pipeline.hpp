// Driving the recorded run: the client, the facilities, and what each step wrote to the trace.
//
// Split from traced_run.hpp so that file is the vocabulary and the publisher, and this one is the
// loop. Both are tool-side: the library records nothing itself, and every event here is built from
// a value a component already returned.

#ifndef DFR_TOOLS_SUPPORT_TRACED_PIPELINE_HPP
#define DFR_TOOLS_SUPPORT_TRACED_PIPELINE_HPP

#include "support/traced_run.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace dfr_tools {

// One driver, so the context stamped onto every event is built in one place and cannot go stale.
class traced_pipeline {
 public:
  traced_pipeline(run_options options, trace_recorder& into)
      : options_(options), trace_(into), client_(trace_client_options(options.lines)) {}

  [[nodiscard]] const run_summary& summary() const { return summary_; }
  [[nodiscard]] trace_client& client() { return client_; }
  [[nodiscard]] std::int64_t now_us() const { return now_us_; }
  void advance(std::int64_t micros) { now_us_ += micros; }

  // Where the run currently is, for stamping onto an event.
  [[nodiscard]] trc::context here() const {
    return trc::context{
        .packet_index = index_,
        .time_ns = now_us_ * 1'000,
        .client_state = static_cast<std::uint8_t>(client_.state()),
        .delivered_through = client_.delivered_through(),
        .messages_missing = client_.total_missing(),
        .outstanding_ranges =
            client_.tracking().outstanding(rec::channel_id::at(0)).size()};
  }

  void set_index(std::uint64_t index) { index_ = index; }

  // Which line the events being recorded came in on. Set once per offered packet rather than
  // threaded through every record() call, because every event of one arrival shares it.
  void set_line(std::size_t line) { line_ = static_cast<std::uint8_t>(line); }

  void record(trc::event_kind kind, rec::sequence_range about = {},
              dfr::error reason = dfr::error::ok, std::uint32_t attempt = 0,
              std::uint64_t detail = 0) {
    auto event = here().with(kind);
    event.line = line_;
    event.first_sequence = about.first;
    event.end_sequence = about.end;
    event.reason = reason;
    event.attempt = attempt;
    event.detail = detail;
    (void)trace_.record(event);
  }

  // Notes the fault the injector applied to this emission, before the packet is offered, so the
  // trace reads cause then effect.
  void note_fault(const chaos::emission& emitted) {
    if (emitted.cause == chaos::fault_op::none && !emitted.is_duplicate) {
      return;
    }
    const auto kind = emitted.is_duplicate ? trc::event_kind::packet_duplicated
                      : emitted.cause == chaos::fault_op::delay
                          ? trc::event_kind::packet_delayed
                          : trc::event_kind::fault_applied;
    record(kind, {}, dfr::error::ok, 0,
           static_cast<std::uint64_t>(emitted.cause));
  }

  // Offers one packet if it survives decoding, recording either the discard or the outcome.
  void offer(dfr::packet_view packet) {
    iex::header header;
    if (iex::decode_header(packet).get(header) != dfr::error::ok) {
      record(trc::event_kind::packet_discarded, {}, dfr::error::truncated_header);
      return;
    }
    if (const auto framing = iex::verify_payload_framing(packet); !framing) {
      record(trc::event_kind::packet_discarded, {}, framing.error_code());
      return;
    }

    const auto before = client_.state();
    rec::ingest_report report;
    const auto outcome =
        client_.on_packet(line_, header.session, header.first_sequence,
                          header.message_count, 0, at_us(now_us_));
    if (outcome.get(report) != dfr::error::ok) {
      record(trc::event_kind::packet_discarded, {}, outcome.error_code());
      note_state_change(before);
      return;
    }

    const rec::sequence_range carried{
        .first = header.first_sequence,
        .end = header.first_sequence + header.message_count};

    switch (report.outcome) {
      case rec::sequencing::gap_opened:
        record(trc::event_kind::gap_opened, report.gap_opened,
               dfr::error::sequence_gap);
        break;
      case rec::sequencing::gap_filled:
        record(trc::event_kind::gap_filled, report.accepted, dfr::error::ok, 0,
               report.recovered);
        break;
      case rec::sequencing::session_reset:
        record(trc::event_kind::session_reset, carried,
               dfr::error::session_changed);
        break;
      case rec::sequencing::duplicate:
        record(trc::event_kind::packet_duplicate, carried);
        break;
      case rec::sequencing::established:
      case rec::sequencing::in_order:
      case rec::sequencing::count_:
        break;
    }

    if (report.held_for_replay) {
      hold(report.accepted);
    } else if (report.delivered()) {
      deliver(report);
      if (report.outcome != rec::sequencing::gap_opened &&
          report.outcome != rec::sequencing::gap_filled) {
        record(trc::event_kind::packet_accepted, report.accepted);
      }
    }
    note_state_change(before);
  }

  // Answers whatever the client is asking for. In glimpse mode nothing is served, which drives it
  // to a snapshot by timing out rather than by being refused.
  void answer(ven::retransmit_facility<512>& facility) {
    for (int step = 0; step < 64; ++step) {
      const auto decision = client_.poll(at_us(now_us_));
      if (decision.what == rec::client_action::idle ||
          decision.what == rec::client_action::request_snapshot ||
          decision.what == rec::client_action::restart) {
        if (decision.what == rec::client_action::request_snapshot) {
          ++summary_.snapshot_requests;
        }
        return;
      }
      if (decision.what != rec::client_action::send_retransmit_request) {
        return;
      }

      ++summary_.retransmit_requests;
      record(trc::event_kind::retransmit_requested, decision.range,
             dfr::error::ok, decision.attempt);
      if (options_.mode == run_mode::glimpse) {
        return;  // the facility is unreachable; the client will time out
      }

      const auto served = facility.serve(
          decision.range, [&](dfr::packet_view packet) { offer(packet); });
      if (!served) {
        ++summary_.retransmit_refusals;
        record(trc::event_kind::retransmit_refused, decision.range,
               served.error_code());
        (void)client_.on_retransmit_refused(decision.range, served.error_code());
        return;
      }
      ++summary_.retransmits_served;
      rec::sequence_range answered{};
      (void)served.get(answered);
      record(trc::event_kind::retransmit_served, answered);
    }
  }

  void finish() {
    summary_.messages_missing = client_.total_missing();
    summary_.final_state = client_.state();
    for (const auto& [sequence, count] : deliveries_) {
      if (count == 1) {
        ++summary_.messages_delivered;
      } else if (count > 1) {
        ++summary_.messages_delivered_twice;
      }
    }
  }

  run_summary& mutable_summary() { return summary_; }
  std::map<std::uint64_t, int>& deliveries() { return deliveries_; }

 private:
  void note_state_change(rec::client_state before) {
    if (client_.state() != before) {
      record(trc::event_kind::state_changed);
    }
  }

  void hold(rec::sequence_range accepted) {
    for (std::uint64_t s = accepted.first; s < accepted.end; ++s) {
      const char body = 'm';
      if (!client_.buffer_message(s, dfr::packet_view{&body, 1})) {
        return;
      }
    }
  }

  void deliver(const rec::ingest_report& report) {
    for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
      ++deliveries_[s];
    }
    for (const auto& repaired : client_.last_recovered().ranges()) {
      for (std::uint64_t s = repaired.first; s < repaired.end; ++s) {
        ++deliveries_[s];
      }
    }
  }

  run_options options_{};
  trace_recorder& trace_;
  trace_client client_;
  run_summary summary_{};
  std::map<std::uint64_t, int> deliveries_{};
  std::int64_t now_us_{0};
  std::uint64_t index_{0};
  std::uint8_t line_{0};
};

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_TRACED_PIPELINE_HPP

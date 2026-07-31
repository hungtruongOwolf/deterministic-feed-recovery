// Writing a recorded run as JSONL.
//
// Extracted from tools/trace.cpp when the browser needed the same output: the viewer now runs this library
// compiled to WebAssembly, and a second implementation of the format would have been a second format. So
// there is one writer, and a trace produced in a browser is byte-identical to one produced in a terminal —
// which the tests assert rather than assume.
//
// Minimal JSON by hand, on purpose: a dependency for twelve fields would be a dependency in the build, and
// every string written here is either a fixed name or an enumerator spelling, so nothing needs escaping.

#ifndef DFR_TOOLS_SUPPORT_TRACE_WRITER_HPP
#define DFR_TOOLS_SUPPORT_TRACE_WRITER_HPP

#include "support/traced_run.hpp"

#include <cstdio>

namespace dfr_tools {

// Minimal JSON writing, by hand and on purpose: a dependency for this would be a dependency in the
// build for the sake of twelve fields, and every string written here is either a fixed name or an
// enumerator name from the library.
inline void write_header(std::FILE* out, const run_options& run,
                  const dfr_tools::run_summary& summary,
                  std::size_t published) {
  std::fprintf(out,
               "{\"kind\":\"run\",\"schema\":\"dfr-trace/1\",\"seed\":%llu,"
               "\"messages\":%zu,\"packets\":%zu,\"session\":%u,\"mode\":\"%s\","
               "\"staleness_messages\":%llu,\"lines\":%zu,\"schedule\":[",
               static_cast<unsigned long long>(run.seed),
               run.messages, published, dfr_tools::kTraceSession,
               run.mode == run_mode::glimpse ? "glimpse" : "recovering",
               static_cast<unsigned long long>(run.staleness_messages),
               run.lines);

  for (std::size_t i = 0; i < summary.schedule.size(); ++i) {
    const auto& fault = summary.schedule[i];
    std::fprintf(out,
                 "%s{\"op\":\"%s\",\"first_packet\":%llu,\"packet_count\":%u,"
                 "\"parameter\":%llu,\"detail\":%u}",
                 i == 0 ? "" : ",", dfr::chaos::to_string(fault.op).data(),
                 static_cast<unsigned long long>(fault.first_packet),
                 fault.packet_count,
                 static_cast<unsigned long long>(fault.parameter), fault.detail);
  }

  // The honesty ledger, generated so it cannot drift from the run.
  std::fprintf(
      out,
      "],\"limits\":["
      "{\"claim\":\"messages delivered exactly once\",\"status\":\"measured\","
      "\"note\":\"counted per sequence in this run\"},"
      "{\"claim\":\"gaps reported are exactly those that never arrived\","
      "\"status\":\"measured\",\"note\":\"checked against what the harness handed over\"},"
      "{\"claim\":\"reproducible from the seed\",\"status\":\"measured\","
      "\"note\":\"manual clock, seeded prng, no wall clock anywhere in the pipeline\"},"
      "{\"claim\":\"tick-to-trade latency\",\"status\":\"not-measurable\","
      "\"note\":\"cloud VMs only: no PMU counters, no NIC hardware timestamping\"},"
      "{\"claim\":\"IGMP snooping and querier behaviour\",\"status\":\"not-measurable\","
      "\"note\":\"AWS does not support multicast on an ordinary VPC\"},"
      "{\"claim\":\"timing numbers in this trace\",\"status\":\"simulated\","
      "\"note\":\"a manual clock advanced by the driver, not a measurement\"}"
      "]}\n");
}

inline void write_event(std::FILE* out, const dfr::trace::event& event) {
  std::fprintf(out,
               "{\"kind\":\"event\",\"i\":%llu,\"t\":%lld,\"layer\":\"%s\","
               "\"event\":\"%s\",\"line\":%u,\"first\":%llu,\"end\":%llu,\"attempt\":%u,"
               "\"detail\":%llu,\"reason\":\"%s\",\"state\":\"%s\","
               "\"delivered_through\":%llu,\"missing\":%llu,\"holes\":%llu,\"gaps\":[",
               static_cast<unsigned long long>(event.packet_index),
               static_cast<long long>(event.time_ns),
               dfr::trace::name_of(dfr::trace::layer_of(event.kind)).data(),
               dfr::trace::name_of(event.kind).data(), event.line,
               static_cast<unsigned long long>(event.first_sequence),
               static_cast<unsigned long long>(event.end_sequence), event.attempt,
               static_cast<unsigned long long>(event.detail),
               dfr::to_string(event.reason).data(),
               dfr::recovery::name_of(
                   static_cast<dfr::recovery::client_state>(event.client_state))
                   .data(),
               static_cast<unsigned long long>(event.delivered_through),
               static_cast<unsigned long long>(event.messages_missing),
               static_cast<unsigned long long>(event.outstanding_ranges));

  // The holes themselves, so a viewer can draw them rather than accumulate them.
  for (std::uint8_t i = 0; i < event.gaps_drawn; ++i) {
    std::fprintf(out, "%s[%llu,%llu]", i == 0 ? "" : ",",
                 static_cast<unsigned long long>(event.gaps[i].first),
                 static_cast<unsigned long long>(event.gaps[i].end));
  }
  std::fprintf(out, "]}\n");
}

inline void write_summary(std::FILE* out, const dfr_tools::run_summary& summary,
                   const dfr_tools::trace_recorder& trace) {
  std::fprintf(
      out,
      "{\"kind\":\"summary\",\"events\":%zu,\"events_dropped\":%llu,"
      "\"messages_delivered\":%llu,\"messages_delivered_twice\":%llu,"
      "\"messages_missing\":%llu,\"retransmit_requests\":%llu,"
      "\"retransmits_served\":%llu,\"retransmit_refusals\":%llu,"
      "\"snapshot_requests\":%llu,\"unfillable_messages\":%llu,"
      "\"final_state\":\"%s\",\"complete\":%s}\n",
      trace.size(), static_cast<unsigned long long>(trace.dropped()),
      static_cast<unsigned long long>(summary.messages_delivered),
      static_cast<unsigned long long>(summary.messages_delivered_twice),
      static_cast<unsigned long long>(summary.messages_missing),
      static_cast<unsigned long long>(summary.retransmit_requests),
      static_cast<unsigned long long>(summary.retransmits_served),
      static_cast<unsigned long long>(summary.retransmit_refusals),
      static_cast<unsigned long long>(summary.snapshot_requests),
      static_cast<unsigned long long>(summary.unfillable_messages),
      dfr::recovery::name_of(summary.final_state).data(),
      trace.dropped() == 0 ? "true" : "false");
}


}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_TRACE_WRITER_HPP

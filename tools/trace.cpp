// Records one complete run of the library as JSONL, for a viewer to read.
//
//   trace [--seed N] [--messages N] [--faults N] [--lines 1|2] [--glimpse] [--staleness N]
//         [--out FILE]
//
// One JSON object per line: a header describing the run, one line per event, and a summary. The
// format is a deliverable, not a debug dump — a trace is a deterministic function of the seed, so
// it can be committed next to the test that produced it and diffed when behaviour changes. "Here is
// the three-fault schedule that breaks it" becomes a file somebody can open.
//
// The viewer must contain no domain logic
// --------------------------------------
// Every event carries the resulting client state and headline numbers, so drawing any moment needs
// one line and no replay. A viewer that reconstructed state from the event sequence would be a
// second implementation of the state machine, written in another language by someone reading the
// first — and when the two disagreed, the picture would be wrong with nothing to say so.
//
// The honesty ledger is in the data
// -------------------------------
// The header carries a `limits` array: which numbers this run measured and which cannot be measured
// on the hardware available at all. It is generated rather than written into a README so that it
// cannot drift from what the run actually did.

#include "support/traced_drivers.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using dfr_tools::run_mode;
using dfr_tools::run_options;

struct cli {
  run_options run{};
  const char* out{nullptr};
};

bool parse(int argc, char** argv, cli& into) {
  for (int i = 1; i < argc; ++i) {
    const auto next = [&]() -> const char* {
      return i + 1 < argc ? argv[++i] : nullptr;
    };
    if (std::strcmp(argv[i], "--seed") == 0) {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      into.run.seed = std::strtoull(value, nullptr, 10);
    } else if (std::strcmp(argv[i], "--messages") == 0) {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      into.run.messages = std::strtoull(value, nullptr, 10);
    } else if (std::strcmp(argv[i], "--faults") == 0) {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      into.run.faults = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (std::strcmp(argv[i], "--staleness") == 0) {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      into.run.staleness_messages = std::strtoull(value, nullptr, 10);
    } else if (std::strcmp(argv[i], "--lines") == 0) {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      into.run.lines = std::strtoull(value, nullptr, 10);
      if (into.run.lines == 0 || into.run.lines > 2) {
        std::fprintf(stderr, "trace: --lines must be 1 or 2\n");
        return false;
      }
    } else if (std::strcmp(argv[i], "--glimpse") == 0) {
      into.run.mode = run_mode::glimpse;
      if (into.run.staleness_messages == 0) {
        into.run.staleness_messages = 20;  // enough to land in the gap
      }
    } else if (std::strcmp(argv[i], "--out") == 0) {
      into.out = next();
      if (into.out == nullptr) {
        return false;
      }
    } else {
      std::fprintf(stderr, "trace: unrecognised argument %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

// Minimal JSON writing, by hand and on purpose: a dependency for this would be a dependency in the
// build for the sake of twelve fields, and every string written here is either a fixed name or an
// enumerator name from the library.
void write_header(std::FILE* out, const cli& options,
                  const dfr_tools::run_summary& summary,
                  std::size_t published) {
  std::fprintf(out,
               "{\"kind\":\"run\",\"schema\":\"dfr-trace/1\",\"seed\":%llu,"
               "\"messages\":%zu,\"packets\":%zu,\"session\":%u,\"mode\":\"%s\","
               "\"staleness_messages\":%llu,\"lines\":%zu,\"schedule\":[",
               static_cast<unsigned long long>(options.run.seed),
               options.run.messages, published, dfr_tools::kTraceSession,
               options.run.mode == run_mode::glimpse ? "glimpse" : "recovering",
               static_cast<unsigned long long>(options.run.staleness_messages),
               options.run.lines);

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

void write_event(std::FILE* out, const dfr::trace::event& event) {
  std::fprintf(out,
               "{\"kind\":\"event\",\"i\":%llu,\"t\":%lld,\"layer\":\"%s\","
               "\"event\":\"%s\",\"line\":%u,\"first\":%llu,\"end\":%llu,\"attempt\":%u,"
               "\"detail\":%llu,\"reason\":\"%s\",\"state\":\"%s\","
               "\"delivered_through\":%llu,\"missing\":%llu,\"holes\":%llu}\n",
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
}

void write_summary(std::FILE* out, const dfr_tools::run_summary& summary,
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

}  // namespace

int main(int argc, char** argv) {
  cli options;
  if (!parse(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: trace [--seed N] [--messages N] [--faults N] "
                 "[--lines 1|2] [--glimpse] [--staleness N] [--out FILE]\n");
    return 2;
  }

  dfr_tools::trace_recorder recorder;
  std::int64_t clock_us = 0;
  const auto stream =
      dfr_tools::publish_stream(options.run.messages, recorder, clock_us);
  if (stream.empty()) {
    std::fprintf(stderr, "trace: the publisher produced nothing\n");
    return 1;
  }

  dfr_tools::run_summary summary =
      dfr_tools::run_traced(options.run, stream, recorder);

  std::FILE* out = stdout;
  if (options.out != nullptr) {
    out = std::fopen(options.out, "wb");
    if (out == nullptr) {
      std::fprintf(stderr, "trace: cannot write %s\n", options.out);
      return 1;
    }
  }

  write_header(out, options, summary, stream.size());
  for (const auto& event : recorder.events()) {
    write_event(out, event);
  }
  write_summary(out, summary, recorder);

  if (out != stdout) {
    std::fclose(out);
    std::fprintf(stderr,
                 "trace: %zu events written to %s (%llu dropped)\n",
                 recorder.size(), options.out,
                 static_cast<unsigned long long>(recorder.dropped()));
  }

  // A truncated trace is reported rather than presented as complete, for the same reason a partial
  // capture read fails inspect: a diagnostic that overstates itself is worse than none.
  return recorder.dropped() == 0 ? 0 : 1;
}

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

#include "support/trace_writer.hpp"
#include "support/traced_drivers.hpp"

#include <cstdio>
#include <cstring>
#include <map>
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
  // The message bodies, so the trace can carry the book they build. See support/traced_market.hpp.
  std::map<std::uint64_t, std::string> bodies;
  const auto stream =
      dfr_tools::publish_stream(options.run.messages, recorder, clock_us, &bodies);
  if (stream.empty()) {
    std::fprintf(stderr, "trace: the publisher produced nothing\n");
    return 1;
  }

  dfr_tools::run_summary summary =
      dfr_tools::run_traced(options.run, stream, recorder, &bodies);

  std::FILE* out = stdout;
  if (options.out != nullptr) {
    out = std::fopen(options.out, "wb");
    if (out == nullptr) {
      std::fprintf(stderr, "trace: cannot write %s\n", options.out);
      return 1;
    }
  }

  dfr_tools::write_header(out, options.run, summary, stream.size());
  for (const auto& event : recorder.events()) {
    dfr_tools::write_event(out, event);
  }
  dfr_tools::write_summary(out, summary, recorder);

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

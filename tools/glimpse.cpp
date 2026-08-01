// glimpse: serve a snapshot, consume it from the bytes, and record what a client rebuilt.
//
// The third defence was the one nobody could watch. In the film it is a plane the escalation marker falls onto, which
// shows the *consequence* of reaching a snapshot and not the snapshot itself. What a snapshot is, and the only thing
// that makes one believable: is that a client with nothing but frames ends up holding the venue's book.
//
// Usage:  glimpse [--levels N] [--resume N] [--trace FILE] [--quiet]

#include "support/glimpse_run.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

int main(int argc, char** argv) {
  std::size_t levels = 5;
  std::uint64_t resume = 4'096;
  const char* trace_path = nullptr;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--levels" && i + 1 < argc) {
      levels = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--resume" && i + 1 < argc) {
      resume = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--trace" && i + 1 < argc) {
      trace_path = argv[++i];
    } else {
      std::fprintf(stderr, "usage: glimpse [--levels N] [--resume N] [--trace FILE] [--quiet]\n");
      return 2;
    }
  }
  if (levels < 1 || levels > 16) {
    std::fprintf(stderr, "glimpse: --levels must be between 1 and 16\n");
    return 2;
  }
  if (resume == 0) {
    std::fprintf(stderr, "glimpse: --resume must be at least 1; a snapshot valid as of nothing is not one\n");
    return 2;
  }

  const auto run = dfr_tools::run_glimpse_session(levels, resume);

  if (trace_path != nullptr) {
    std::FILE* out = std::fopen(trace_path, "w");
    if (out == nullptr) {
      std::fprintf(stderr, "glimpse: cannot write %s\n", trace_path);
      return 1;
    }
    dfr_tools::write_glimpse_header(
        out, dfr_tools::kTracedSymbol, dfr_tools::kGlimpseSession,
        static_cast<std::uint16_t>(run.venue.bids().size()),
        static_cast<std::uint16_t>(run.venue.asks().size()), run.venue.bids().best().at.raw(),
        run.venue.bids().best().size, run.venue.asks().best().at.raw(),
        run.venue.asks().best().size, run.resume_from);
    for (const auto& step : run.steps) {
      dfr_tools::write_glimpse_step(out, step);
    }
    std::fclose(out);
    if (!quiet) {
      std::printf("wrote %s: %zu frames\n", trace_path, run.steps.size());
    }
  }

  if (!quiet) {
    std::printf("\nA Glimpse snapshot, rebuilt by a client that has only the bytes\n\n");
    std::printf("  %-5s %-16s %-38s %s\n", "type", "frame", "what it carries", "the client's book");
    for (const auto& step : run.steps) {
      // 80, not 48: two four-decimal prices, two level counts and the separators can exceed 48, and GCC does the
      // arithmetic. The second buffer-size warning this project has had from it.
      char book[80] = "empty";
      if (step.bid_levels > 0 || step.ask_levels > 0) {
        std::snprintf(book, sizeof book, "%lld.%04lld / %lld.%04lld  %u+%u lvl",
                      static_cast<long long>(step.bid / 10'000),
                      static_cast<long long>(step.bid % 10'000),
                      static_cast<long long>(step.ask / 10'000),
                      static_cast<long long>(step.ask % 10'000), step.bid_levels,
                      step.ask_levels);
      }
      std::printf("  %-5c %-16.*s %-38s %s%s\n", step.type, static_cast<int>(step.name.size()),
                  step.name.data(), step.detail.c_str(), book, step.matches ? "  = venue" : "");
    }
    std::printf("\n  the client's book %s the venue's, built from frames alone\n",
                run.matched ? "equals" : "DOES NOT equal");
    std::printf("  resume the live feed at %llu, the *next* message, not the last included\n\n",
                static_cast<unsigned long long>(run.resume_from));
  }
  return run.matched ? 0 : 1;
}

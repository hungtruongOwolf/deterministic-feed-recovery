// Reporting a benchmark run, and counting what it allocated.
//
// Split from recovery_bench.cpp at the seam it was over its length limit across, and it is a real one: a reader
// checking whether a *measurement* is honest never needs the JSON writer, and the JSON writer has no opinion about
// what is being measured.
//
// The allocation counter lives here too, because it is the same subject — reporting a property of the run rather
// than timing an operation. Replacing the global `operator new` is the only way to check "no allocation after
// initialisation" that cannot be fooled: it catches an allocation anywhere below, including one a container makes
// on a path nobody thought about.

#ifndef DFR_BENCH_SUPPORT_REPORT_HPP
#define DFR_BENCH_SUPPORT_REPORT_HPP

#include "support/measure.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace dfr_bench {

// Armed only around the measured section, so the harness's own vectors are not attributed to the library.
inline std::uint64_t g_allocations = 0;
inline bool g_counting = false;

inline void print_row(const result& r) {
  std::printf("  %-38s %9.1f %9.1f %9.1f %9.1f   %12.0f/s\n", r.name.c_str(), r.best, r.p50, r.p99,
              r.worst, r.per_second());
}

inline void write_json(std::FILE* out, const std::vector<result>& results,
                std::string_view assertions, std::uint64_t allocations) {
  std::fprintf(out,
               "{\"kind\":\"benchmarks\",\"schema\":\"dfr-bench/1\",\"assertions\":\"%.*s\","
               "\"allocations_after_init\":%llu,\"measurements\":[",
               static_cast<int>(assertions.size()), assertions.data(),
               static_cast<unsigned long long>(allocations));
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    std::fprintf(out,
                 "%s{\"name\":\"%s\",\"unit\":\"%s\",\"batch\":%zu,\"samples\":%zu,"
                 "\"best_ns\":%.3f,\"p50_ns\":%.3f,\"p99_ns\":%.3f,\"worst_ns\":%.3f,"
                 "\"mean_ns\":%.3f,\"per_second\":%.0f}",
                 i == 0 ? "" : ",", r.name.c_str(), r.unit.c_str(), r.batch, r.samples, r.best,
                 r.p50, r.p99, r.worst, r.mean, r.per_second());
  }
  // The ledger travels with the numbers, so a page cannot show a throughput figure as a latency one.
  std::fprintf(out,
               "],\"limits\":["
               "{\"claim\":\"nanoseconds per operation on this machine\",\"status\":\"measured\","
               "\"note\":\"steady_clock over batches; percentiles are over batch means, not over "
               "individual operations\"},"
               "{\"claim\":\"allocations after initialisation\",\"status\":\"measured\","
               "\"note\":\"global operator new counted across a whole recovery run\"},"
               "{\"claim\":\"tick-to-trade latency\",\"status\":\"not-measurable\","
               "\"note\":\"no PMU counters and no NIC hardware timestamping on the machines this runs "
               "on; a figure produced anyway would have nothing behind it\"},"
               "{\"claim\":\"how this compares to a production feed handler\",\"status\":"
               "\"not-measurable\",\"note\":\"no published figures to compare against, and a "
               "different machine would move every number here\"}"
               "]}\n");
}


}  // namespace dfr_bench

#endif  // DFR_BENCH_SUPPORT_REPORT_HPP

// Measuring, without lying about what was measured.
//
// Written rather than taken from a library, and the reason is the one this whole project is about: a
// benchmark that reports a mean is a benchmark that hides its tail, and a tail is the only part of a
// latency figure anybody trading cares about. So this reports percentiles, and it is explicit about
// *what* the percentiles are over.
//
// The honest description of the numbers below
// -------------------------------------------
// `steady_clock` on this machine resolves to tens of nanoseconds, and several of the operations here cost
// less than that. Timing one of them individually would measure the clock. So each sample times a *batch*
// of `batch` operations and divides, which means:
//
//   * the reported figure is a **per-operation cost within a batch**, and
//   * the percentiles are over **batch means**, not over individual operations.
//
// That is a weaker statement than a true per-operation p99 and it is the statement the hardware supports.
// A p99 over batch means cannot show a single 10-microsecond stall inside a batch of a thousand; it can show
// that some batches ran consistently slower than others, which is what scheduler noise and cache state look
// like. Calling it what it is costs nothing and lets somebody reading it know which conclusions are theirs
// to draw. See docs/BENCHMARKS.md.
//
// Nothing here allocates during measurement: the sample vector is reserved up front, because a benchmark
// that allocated between samples would be measuring the allocator.

#ifndef DFR_BENCH_SUPPORT_MEASURE_HPP
#define DFR_BENCH_SUPPORT_MEASURE_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dfr_bench {

// Stops the compiler deleting work whose result nobody reads.
//
// The empty asm with a memory clobber is the standard barrier and it is the whole reason these numbers are
// not zero: without it, `-O3 -flto` is entirely within its rights to notice that a decoded packet is never
// looked at and remove the decode. A benchmark that measures an optimised-away loop reports impressive
// figures for nothing at all.
template <typename T>
inline void keep(T&& value) noexcept {
  asm volatile("" : : "r,m"(value) : "memory");
}

inline void barrier() noexcept { asm volatile("" : : : "memory"); }

struct result {
  std::string name;
  // What one operation is, so a reader knows what the nanoseconds are per. "one packet framed and decoded",
  // not "iteration".
  std::string unit;
  std::size_t batch{0};
  std::size_t samples{0};

  // Nanoseconds per operation, from batch means. See the header comment for why that phrasing matters.
  double best{0};
  double p50{0};
  double p99{0};
  double worst{0};
  double mean{0};

  [[nodiscard]] double per_second() const noexcept {
    return p50 > 0 ? 1e9 / p50 : 0;
  }
};

// Runs `work(batch)` repeatedly and reports the distribution of its per-operation cost.
//
// `work` takes the batch size and returns something; the return is passed through `keep` so the compiler
// cannot decide the loop was pointless. A void-returning work function would need the caller to remember the
// barrier, and a benchmark harness that can be used wrongly will be.
template <typename Work>
result measure(std::string_view name, std::string_view unit, std::size_t batch,
               std::size_t samples, Work&& work) {
  std::vector<double> per_op;
  per_op.reserve(samples);

  // Warm up: the first pass pays for cold instruction cache and the first branch predictions, and reporting
  // that as the best case would be reporting a number no steady state ever sees.
  for (std::size_t i = 0; i < 3; ++i) {
    keep(work(batch));
  }

  for (std::size_t i = 0; i < samples; ++i) {
    barrier();
    const auto start = std::chrono::steady_clock::now();
    keep(work(batch));
    const auto end = std::chrono::steady_clock::now();
    barrier();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    per_op.push_back(static_cast<double>(nanos) / static_cast<double>(batch));
  }

  std::sort(per_op.begin(), per_op.end());
  const auto at = [&](double q) {
    const auto index = static_cast<std::size_t>(q * static_cast<double>(per_op.size() - 1));
    return per_op[index];
  };

  double total = 0;
  for (const double value : per_op) {
    total += value;
  }

  return result{.name = std::string{name},
                .unit = std::string{unit},
                .batch = batch,
                .samples = samples,
                .best = per_op.front(),
                .p50 = at(0.50),
                .p99 = at(0.99),
                .worst = per_op.back(),
                .mean = total / static_cast<double>(per_op.size())};
}

}  // namespace dfr_bench

#endif  // DFR_BENCH_SUPPORT_MEASURE_HPP

// What it costs to hand a message to another core.
//
// The recovery path's own cost is measured in recovery_bench. This measures the seam: one thread producing
// deliveries into an SPSC ring and another consuming them. It is the only number in this project that
// involves two cores, and it is the number a feed handler's architecture actually turns on — because the
// hand-off is where a badly built one loses an order of magnitude to false sharing.
//
// Three things are measured, and the third is the one people forget:
//
//   1. the round trip with an idle consumer, which is the best case;
//   2. the same with a batched drain, which amortises one acquire over many records;
//   3. what happens when the consumer cannot keep up — because a ring that refuses is only a good design if
//      you can say how often it refused.
//
// Wall-clock throughput here, not per-operation batch means: two threads make the batch-mean trick
// meaningless, since the producer's cost and the consumer's overlap. So this reports messages per second and
// nanoseconds per message derived from it, which is what the number means anyway.

#include "support/measure.hpp"

#include <dfr/concurrent/delivery.hpp>
#include <dfr/core/narrow.hpp>
#include <dfr/concurrent/spsc_ring.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace conc = dfr::concurrent;
namespace bench = dfr_bench;

namespace {

using ring_type = conc::spsc_ring<conc::delivery, 8192>;

// The same ring with the padding removed, and nothing else changed.
//
// This exists to price the single most expensive mistake available in this structure. When the producer's
// index and the consumer's index share a cache line, every push invalidates the line the consumer is reading
// its own index from and every pop returns the favour — so the two cores spend their time passing one line
// back and forth instead of moving data. It is invisible in the source, invisible to a test, and it is the
// reason `alignas` appears in spsc_ring.hpp with a paragraph next to it.
//
// Kept in the benchmark rather than in the library: it is a measurement instrument, not an option anybody
// should be able to select by accident.
template <typename T, std::size_t Capacity>
class unpadded_ring {
 public:
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
  [[nodiscard]] std::uint64_t refused() const noexcept {
    return refused_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool push(const T& value) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) >= Capacity) {
      refused_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slots_[tail & (Capacity - 1)] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool pop(T& into) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head >= tail_.load(std::memory_order_acquire)) {
      return false;
    }
    into = slots_[head & (Capacity - 1)];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t pop_batch(T* out, std::size_t limit) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    const auto available = dfr::narrowed<std::size_t>(tail - head);
    const std::size_t taking = available < limit ? available : limit;
    for (std::size_t i = 0; i < taking; ++i) {
      out[i] = slots_[(head + i) & (Capacity - 1)];
    }
    if (taking > 0) {
      head_.store(head + taking, std::memory_order_release);
    }
    return taking;
  }

 private:
  // The whole point: adjacent, so they share a line. No cached copies either, so every operation reads the
  // other side's index.
  std::atomic<std::uint64_t> tail_{0};
  std::atomic<std::uint64_t> head_{0};
  std::atomic<std::uint64_t> refused_{0};
  std::array<T, Capacity> slots_{};
};

using unpadded_type = unpadded_ring<conc::delivery, 8192>;

// The same comparison over an 8-byte record.
//
// The first attempt at pricing false sharing used the 272-byte delivery record and found the unpadded ring
// *faster* — which is not a result about padding, it is a result about what dominates. Copying 272 bytes
// costs more than the cache line the two indices fight over, so the fight is invisible underneath it.
//
// Isolating it needs a record small enough that the index traffic is the whole cost. That is also the honest
// framing of when the padding earns its keep: it matters when the records are small and the ring is hot, and
// it is noise when each record is a quarter of a kilobyte. A design note that says "pad the indices" without
// saying when is a cargo cult.
using small_padded = conc::spsc_ring<std::uint64_t, 8192>;
using small_unpadded = unpadded_ring<std::uint64_t, 8192>;

struct handoff_result {
  std::string name;
  std::string note;
  double nanos_per_message{0};
  double messages_per_second{0};
  std::uint64_t refused{0};
};

// One record of whichever type the ring carries, so the delivery rows and the 8-byte rows share `run`.
template <typename Record>
Record a_record(std::uint64_t sequence);

template <>
conc::delivery a_record<conc::delivery>(std::uint64_t sequence) {
  conc::delivery record;
  const std::array<std::byte, 40> body{};
  (void)conc::delivery::from(sequence, static_cast<std::uint8_t>(sequence % 2), sequence % 5 == 0,
                             dfr::packet_view{body.data(), body.size()}, record);
  return record;
}

template <>
std::uint64_t a_record<std::uint64_t>(std::uint64_t sequence) {
  return sequence;
}

// One producer, one consumer, `count` messages, and the wall clock around both.
//
// `drain` is how the consumer takes them, so the batched and unbatched cases share everything else. `spin`
// says whether a full ring makes the producer wait or give up, which is the difference between measuring the
// hand-off and measuring backpressure.
template <typename Ring = ring_type, typename Record = conc::delivery, typename Drain>
handoff_result run(std::string_view name, std::string_view note, std::uint64_t count, bool spin,
                   Drain&& drain) {
  Ring ring;
  std::atomic<bool> consumer_ready{false};
  std::atomic<std::uint64_t> consumed{0};

  std::thread consumer([&] {
    consumer_ready.store(true, std::memory_order_release);
    std::uint64_t seen = 0;
    std::uint64_t checksum = 0;
    while (seen < count) {
      seen += drain(ring, checksum);
    }
    bench::keep(checksum);
    consumed.store(seen, std::memory_order_release);
  });

  while (!consumer_ready.load(std::memory_order_acquire)) {
  }

  const auto start = std::chrono::steady_clock::now();
  std::uint64_t offered = 0;
  for (std::uint64_t i = 1; i <= count; ++i) {
    const auto record = a_record<Record>(i);
    if (spin) {
      while (!ring.push(record)) {
      }
    } else if (!ring.push(record)) {
      continue;  // refused, and counted by the ring
    }
    ++offered;
  }
  // If the producer was allowed to give up, the consumer is waiting for messages that will never come. Top it
  // up so the thread can finish; those are excluded from the timing, which stops here.
  const auto end = std::chrono::steady_clock::now();
  while (consumed.load(std::memory_order_acquire) < count) {
    if (!ring.push(a_record<Record>(0))) {
      std::this_thread::yield();
    }
  }
  consumer.join();

  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  const double per_message = static_cast<double>(nanos) / static_cast<double>(offered);
  return handoff_result{.name = std::string{name},
                        .note = std::string{note},
                        .nanos_per_message = per_message,
                        .messages_per_second = per_message > 0 ? 1e9 / per_message : 0,
                        .refused = ring.refused()};
}

void write_json(std::FILE* out, const std::vector<handoff_result>& results,
                std::size_t capacity) {
  std::fprintf(out,
               "{\"kind\":\"handoff\",\"schema\":\"dfr-handoff/1\",\"ring_capacity\":%zu,"
               "\"measurements\":[",
               capacity);
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    std::fprintf(out,
                 "%s{\"name\":\"%s\",\"note\":\"%s\",\"ns_per_message\":%.3f,"
                 "\"messages_per_second\":%.0f,\"refused\":%llu}",
                 i == 0 ? "" : ",", r.name.c_str(), r.note.c_str(), r.nanos_per_message,
                 r.messages_per_second, static_cast<unsigned long long>(r.refused));
  }
  std::fprintf(
      out,
      "],\"limits\":["
      "{\"claim\":\"messages per second across a thread boundary\",\"status\":\"measured\","
      "\"note\":\"wall clock around a producer thread, two cores of one machine\"},"
      "{\"claim\":\"the ring refuses rather than overwriting when the consumer falls behind\","
      "\"status\":\"measured\",\"note\":\"the refusal count is the ring's own, and the slow-consumer "
      "row exercises it\"},"
      "{\"claim\":\"the memory ordering is sufficient\",\"status\":\"not-measurable\","
      "\"note\":\"ThreadSanitizer does not model relaxed atomics precisely and passes a deliberately "
      "broken version of this ring; what catches it is the property test on weakly-ordered hardware, "
      "which failed 12 times out of 12. See docs/CONCURRENCY.md\"},"
      "{\"claim\":\"how this compares to a production hand-off\",\"status\":\"not-measurable\","
      "\"note\":\"no published figures to compare against, and core-to-core cost is a property of the "
      "machine as much as of the code\"}"
      "]}\n");
}

}  // namespace

int main(int argc, char** argv) {
  const char* json_path = nullptr;
  std::uint64_t count = 4'000'000;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
      json_path = argv[++i];
    } else if (std::strcmp(argv[i], "--messages") == 0 && i + 1 < argc) {
      count = std::strtoull(argv[++i], nullptr, 10);
    } else {
      std::fprintf(stderr, "usage: handoff_bench [--messages N] [--json FILE]\n");
      return 2;
    }
  }

  std::vector<handoff_result> results;

  results.push_back(run("one at a time", "pop() per message, consumer keeping up", count, true,
                        [](ring_type& ring, std::uint64_t& checksum) -> std::uint64_t {
                          conc::delivery record;
                          if (!ring.pop(record)) {
                            return 0;
                          }
                          checksum += record.sequence;
                          return 1;
                        }));

  results.push_back(run("drained in batches of 64", "pop_batch(), one acquire per batch", count,
                        true, [](ring_type& ring, std::uint64_t& checksum) -> std::uint64_t {
                          std::array<conc::delivery, 64> batch{};
                          const auto got = ring.pop_batch(batch.data(), batch.size());
                          for (std::size_t i = 0; i < got; ++i) {
                            checksum += batch[i].sequence;
                          }
                          return got;
                        }));

  // The row that matters for the design decision: the consumer is deliberately slow, the ring fills, and the
  // producer is refused rather than silently overwriting. The refusal count is the point.
  // The cost of the mistake, measured rather than asserted. Same protocol, same records, same batch size —
  // only the padding differs.
  results.push_back(run<unpadded_type>(
      "unpadded, one at a time", "the two indices sharing a cache line", count, true,
      [](unpadded_type& ring, std::uint64_t& checksum) -> std::uint64_t {
        conc::delivery record;
        if (!ring.pop(record)) {
          return 0;
        }
        checksum += record.sequence;
        return 1;
      }));

  // Small records, where the index traffic is the whole cost and the padding is the difference.
  results.push_back(run<small_padded, std::uint64_t>(
      "8-byte records, padded", "one cache line per index", count, true,
      [](small_padded& ring, std::uint64_t& checksum) -> std::uint64_t {
        std::uint64_t value = 0;
        if (!ring.pop(value)) {
          return 0;
        }
        checksum += value;
        return 1;
      }));

  results.push_back(run<small_unpadded, std::uint64_t>(
      "8-byte records, unpadded", "both indices on one line — the mistake", count, true,
      [](small_unpadded& ring, std::uint64_t& checksum) -> std::uint64_t {
        std::uint64_t value = 0;
        if (!ring.pop(value)) {
          return 0;
        }
        checksum += value;
        return 1;
      }));

  results.push_back(run("consumer falling behind", "producer refused rather than overwriting",
                        count / 8, false,
                        [](ring_type& ring, std::uint64_t& checksum) -> std::uint64_t {
                          conc::delivery record;
                          if (!ring.pop(record)) {
                            return 0;
                          }
                          // Enough work that the consumer genuinely cannot keep up. The first version did
                          // forty exclusive-ors, the ring of 8,192 absorbed all of it, and the row reported
                          // zero refusals — a "consumer falling behind" measurement in which nobody fell
                          // behind. The number is only worth printing if the condition it names occurs.
                          for (int i = 0; i < 3'000; ++i) {
                            checksum += record.sequence ^ static_cast<std::uint64_t>(i);
                            bench::keep(checksum);
                          }
                          return 1;
                        }));

  std::printf("\ndfr thread hand-off · SPSC ring of %zu records · %llu messages\n",
              ring_type::capacity(), static_cast<unsigned long long>(count));
  std::printf("wall clock across two cores of one machine — not a wire latency\n\n");
  std::printf("  %-28s %12s %18s %12s\n", "", "ns/message", "messages/s", "refused");
  for (const auto& r : results) {
    std::printf("  %-28s %12.1f %18.0f %12llu\n", r.name.c_str(), r.nanos_per_message,
                r.messages_per_second, static_cast<unsigned long long>(r.refused));
  }
  std::printf("\n  the third row is the design decision: a full ring refuses and counts it, rather "
              "than\n  overwriting the oldest record and turning a known backlog into a silent "
              "hole.\n\n");

  if (json_path != nullptr) {
    std::FILE* out = std::fopen(json_path, "w");
    if (out == nullptr) {
      std::fprintf(stderr, "handoff_bench: cannot write %s\n", json_path);
      return 1;
    }
    write_json(out, results, ring_type::capacity());
    std::fclose(out);
    std::printf("wrote %s\n", json_path);
  }
  return 0;
}

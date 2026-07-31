// What the recovery path costs, measured.
//
// The project had a preset called `bench` that turned assertions off and measured nothing. For a library
// whose reason to exist is a hot path that runs when something has gone wrong, that was the largest hole in
// it: "no allocation after init" and "poll-driven" were design claims with no number attached.
//
// What is measured, and what is deliberately not
// ----------------------------------------------
// Measured: the cost of the operations the recovery path performs — framing a datagram, decoding a header,
// the gap-set arithmetic, one client poll, and a whole feed end to end. Nanoseconds per operation, reported
// as best / p50 / p99 / worst so the tail is visible rather than averaged away.
//
// Not measured, and not measurable here: tick-to-trade latency, NIC-to-NIC latency, or anything requiring a
// hardware timestamp. This runs on a cloud VM or a laptop with no PMU counters and no NIC timestamping, and
// a figure produced anyway would be a number with nothing behind it. That distinction is in the JSON output
// as well, so a page drawing these cannot present one as the other.
//
// The comparison that *is* honest and is the interesting one: assertions on versus off. Building the same
// benchmark under the dev and bench presets prices the paranoia, which is a real engineering number and one
// nobody can get from a design document.

#include "support/measure.hpp"

#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/schedule.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/recovery/gap_set.hpp>
#include <dfr/venue/publisher.hpp>
#include <dfr/wire/iextp/header.hpp>
#include <dfr/wire/moldudp64/cursor.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bench = dfr_bench;
namespace iex = dfr::wire::iextp;
namespace mold = dfr::wire::moldudp64;
namespace rec = dfr::recovery;
namespace ven = dfr::venue;

namespace {

using clock_type = dfr::manual_clock;
using bench_client = rec::client<clock_type, rec::replay_buffer<16'384, 2048>>;

// A published feed to decode, built once. Building it inside a benchmark would measure the publisher.
struct feed {
  std::vector<std::string> packets;
  std::vector<std::uint64_t> first_sequence;
  std::vector<std::uint64_t> message_count;
};

feed publish(std::size_t messages) {
  ven::publisher_options options;
  options.session = 0xBEEF;
  ven::iextp_publisher<clock_type> publisher{options};

  feed out;
  clock_type clock;
  std::size_t produced = 0;
  std::uint64_t body = 0;

  const auto capture = [&](dfr::packet_view packet) {
    out.packets.emplace_back(reinterpret_cast<const char*>(packet.data()), packet.size());
    iex::header decoded{};
    if (iex::decode_header(packet).get(decoded) == dfr::error::ok) {
      out.first_sequence.push_back(decoded.first_sequence);
      out.message_count.push_back(decoded.message_count);
    } else {
      out.first_sequence.push_back(0);
      out.message_count.push_back(0);
    }
  };

  // No open() needed: the publisher opens its session on the first submit.
  while (produced < messages) {
    // Eight messages then a flush, so the packets carry several blocks each: a benchmark over
    // single-message packets would measure framing and call it decoding.
    for (std::size_t i = 0; i < 8 && produced < messages; ++i) {
      std::array<std::byte, 40> message{};
      for (std::size_t at = 0; at < message.size(); ++at) {
        message[at] = static_cast<std::byte>((body + at) & 0xFF);
      }
      ++body;
      ++produced;
      (void)publisher.submit(dfr::packet_view{message.data(), message.size()}, clock.now(), capture);
    }
    (void)publisher.flush(clock.now(), capture);
    clock.advance(std::chrono::microseconds{50});
  }
  return out;
}

rec::client_options client_options() {
  rec::client_options options;
  options.lines = 1;
  return options;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void print_row(const bench::result& r) {
  std::printf("  %-38s %9.1f %9.1f %9.1f %9.1f   %12.0f/s\n", r.name.c_str(), r.best, r.p50, r.p99,
              r.worst, r.per_second());
}

void write_json(std::FILE* out, const std::vector<bench::result>& results,
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

}  // namespace

// ---------------------------------------------------------------------------
// Counting allocations
// ---------------------------------------------------------------------------
//
// The library claims it allocates nothing after initialisation. That claim was in prose and never checked.
// Replacing the global operator new is the only way to check it that cannot be fooled: it catches an
// allocation anywhere below, including one a container makes on a path nobody thought about.
//
// Defined at file scope because a replacement operator new must be; the counter is only read after the
// measured section, and this program is single-threaded, so a plain integer is honest here.
namespace {
std::uint64_t g_allocations = 0;
bool g_counting = false;
}  // namespace

void* operator new(std::size_t size) {
  if (g_counting) {
    ++g_allocations;
  }
  void* memory = std::malloc(size == 0 ? 1 : size);
  if (memory == nullptr) {
    std::abort();  // no exceptions in this build
  }
  return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main(int argc, char** argv) {
  const char* json_path = nullptr;
  std::size_t samples = 200;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
      json_path = argv[++i];
    } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
      samples = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr, "usage: recovery_bench [--samples N] [--json FILE]\n");
      return 2;
    }
  }

  const feed stream = publish(4'096);
  std::vector<bench::result> results;

  // ---- the wire ----------------------------------------------------------

  results.push_back(bench::measure(
      "decode an IEX-TP header", "one 40-byte header", 1'024, samples, [&](std::size_t batch) {
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < batch; ++i) {
          const auto& bytes = stream.packets[i % stream.packets.size()];
          iex::header decoded{};
          if (iex::decode_header(dfr::packet_view{bytes.data(), bytes.size()}).get(decoded) ==
              dfr::error::ok) {
            sum += decoded.first_sequence + decoded.message_count;
          }
        }
        return sum;
      }));

  results.push_back(bench::measure(
      "frame and walk a MoldUDP64 packet", "one packet, every message", 512, samples,
      [&](std::size_t batch) {
        std::uint64_t bytes_seen = 0;
        for (std::size_t i = 0; i < batch; ++i) {
          const auto& bytes = stream.packets[i % stream.packets.size()];
          auto cursor = mold::message_cursor::over(dfr::packet_view{bytes.data(), bytes.size()});
          if (!cursor) {
            continue;
          }
          auto walking = cursor.value();
          while (!walking.done()) {
            const auto next = walking.next();
            if (!next) {
              break;
            }
            bytes_seen += next.value().payload.size();
          }
        }
        return bytes_seen;
      }));

  // ---- the gap arithmetic ------------------------------------------------
  //
  // The operation the recovery path performs most and the one a naive implementation gets quadratic.

  // A whole hole's life, and the unit says so.
  //
  // The first version of this timed a fresh eight-hole set being *built* inside the loop and then divided by
  // one, reporting ~950 ns for a single `open`. That is nine operations plus a construction called one, and
  // the number was wrong by an order of magnitude in the flattering direction for the wrong reason — it
  // looked slow, but it was measuring the wrong thing, which is the same defect either way.
  //
  // So the unit is the whole cycle: eight holes opened, then all eight filled. Sixteen operations, divided by
  // sixteen. That is also what actually happens on a feed, so the number means something.
  results.push_back(bench::measure(
      "gap-set arithmetic, a hole's whole life", "one open or one fill, over 8 holes", 128, samples,
      [&](std::size_t batch) {
        std::uint64_t total = 0;
        const std::size_t cycles = std::max<std::size_t>(1, batch / 16);
        for (std::size_t i = 0; i < cycles; ++i) {
          // Offset by the running total, so no iteration is a copy of the last one. Without this the
          // optimiser folds all `cycles` iterations into one and the benchmark reports 0.3 ns for sixteen
          // set operations — a number that describes the compiler, not the code.
          const std::uint64_t base = 1 + (total & 0xFFF);
          rec::gap_set holes;
          for (std::uint64_t h = 0; h < 8; ++h) {
            (void)holes.open({.first = base + h * 100, .end = base + h * 100 + 10});
          }
          total += holes.total_missing();
          for (std::uint64_t h = 0; h < 8; ++h) {
            std::uint64_t filled = 0;
            (void)holes.fill({.first = base + h * 100, .end = base + h * 100 + 10}).get(filled);
            total += filled;
          }
          bench::keep(holes.size());
        }
        return total;
      }));

  // ---- the client --------------------------------------------------------

  // The recovering hot path: a feed with loss in it, ingested and polled.
  //
  // Two earlier versions of this were both mislabelled, and both in the flattering direction until the
  // number was read properly:
  //
  //   1. polling a synchronised client reported 0.0 ns and ninety-seven billion polls a second. The state
  //      never changed, so the loop was invariant and the optimiser kept one answer.
  //   2. constructing a fresh client each iteration reported ~594 ns for "one poll" — which was mostly the
  //      cost of zero-initialising a 53 KB object, not of polling.
  //
  // So: one client, one long feed, every seventh packet dropped so holes open and close, and a poll after
  // each. The unit is one packet plus one poll, which is what the loop actually does and what a receiver
  // actually does. Nothing is invariant because the client's state advances with the feed.
  results.push_back(bench::measure(
      "ingest with loss, and poll", "one packet plus one poll, while recovering", 512, samples,
      [&](std::size_t batch) {
        bench_client client{client_options()};
        clock_type clock;
        std::uint64_t work = 0;
        for (std::size_t i = 0; i < batch; ++i) {
          const std::size_t at = i % stream.packets.size();
          if (at % 7 == 3) {
            clock.advance(std::chrono::microseconds{50});
            continue;  // the loss the recovery path exists for
          }
          const auto& bytes = stream.packets[at];
          iex::header decoded{};
          if (iex::decode_header(dfr::packet_view{bytes.data(), bytes.size()}).get(decoded) !=
              dfr::error::ok) {
            continue;
          }
          const auto report =
              client.on_packet(0, decoded.session, decoded.first_sequence, decoded.message_count,
                               decoded.first_sequence, clock.now());
          if (report) {
            work += report.value().recovered;
          }
          const auto decision = client.poll(clock.now());
          work += static_cast<std::uint64_t>(decision.what);
          bench::keep(decision.range.first);
          clock.advance(std::chrono::microseconds{50});
        }
        return work;
      }));

  // The end-to-end number, and the one worth quoting: a whole feed through the client, in order, with the
  // wire decode included. This is "messages per second the recovery path can absorb".
  results.push_back(bench::measure(
      "ingest a packet end to end", "one packet: decode, arbitrate, track", 256, samples,
      [&](std::size_t batch) {
        bench_client client{client_options()};
        clock_type clock;
        std::uint64_t delivered = 0;
        for (std::size_t i = 0; i < batch; ++i) {
          // Walks the real feed in order, so the sequence advances and nothing is loop-invariant.
          const std::size_t at = i % stream.packets.size();
          const auto& bytes = stream.packets[at];
          iex::header decoded{};
          if (iex::decode_header(dfr::packet_view{bytes.data(), bytes.size()}).get(decoded) !=
              dfr::error::ok) {
            continue;
          }
          const auto report =
              client.on_packet(0, decoded.session, decoded.first_sequence, decoded.message_count,
                               decoded.first_sequence, clock.now());
          if (report) {
            delivered += report.value().accepted.end - report.value().accepted.first;
          }
          clock.advance(std::chrono::microseconds{50});
        }
        return delivered;
      }));

  // ---- the allocation claim ---------------------------------------------
  //
  // A whole run, with the counter armed. Everything above ran before this, so the vectors the harness itself
  // needed are already grown and cannot be attributed to the library.
  g_allocations = 0;
  g_counting = true;
  {
    bench_client client{client_options()};
    clock_type clock;
    std::uint64_t delivered = 0;
    for (std::size_t i = 0; i < stream.packets.size(); ++i) {
      const auto& bytes = stream.packets[i];
      iex::header decoded{};
      if (iex::decode_header(dfr::packet_view{bytes.data(), bytes.size()}).get(decoded) !=
          dfr::error::ok) {
        continue;
      }
      const auto report = client.on_packet(0, decoded.session, decoded.first_sequence,
                                           decoded.message_count, decoded.first_sequence,
                                           clock.now());
      if (report) {
        delivered += report.value().accepted.end - report.value().accepted.first;
      }
      (void)client.poll(clock.now());
      clock.advance(std::chrono::microseconds{50});
    }
    bench::keep(delivered);
  }
  g_counting = false;

#ifdef NDEBUG
  const std::string_view assertions = "off";
#else
  const std::string_view assertions = "on";
#endif

  std::printf("\ndfr recovery benchmarks · assertions %s · %zu samples per measurement\n",
              assertions.data(), samples);
  std::printf("nanoseconds per operation, from batch means — see docs/BENCHMARKS.md for what that "
              "does and does not say\n\n");
  std::printf("  %-38s %9s %9s %9s %9s   %14s\n", "", "best", "p50", "p99", "worst", "rate");
  for (const auto& r : results) {
    print_row(r);
  }
  std::printf("\n  allocations after initialisation: %llu",
              static_cast<unsigned long long>(g_allocations));
  std::printf(g_allocations == 0 ? "  ✓\n" : "  ← the claim is broken\n");
  std::printf("\n  not measured here: tick-to-trade, NIC-to-NIC, anything needing a hardware "
              "timestamp.\n  This machine has no PMU counters and no NIC timestamping, so no figure "
              "is given rather than\n  a figure with nothing behind it.\n\n");

  if (json_path != nullptr) {
    std::FILE* out = std::fopen(json_path, "w");
    if (out == nullptr) {
      std::fprintf(stderr, "recovery_bench: cannot write %s\n", json_path);
      return 1;
    }
    write_json(out, results, assertions, g_allocations);
    std::fclose(out);
    std::printf("wrote %s\n", json_path);
  }

  return g_allocations == 0 ? 0 : 1;
}

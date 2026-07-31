// Runs the whole pipeline over a real capture and checks it accounts for itself.
//
// This is tests/integration/recovery_oracle_test.cpp pointed at real data. The unit oracle
// runs in CI over synthetic packets, which is the right place for it: fast, self-contained,
// and it fails on the commit that broke something. This tool answers the harder question —
// does the same hold on a day of actual exchange traffic, with its real message-size
// distribution, its heartbeats, its quiet periods and its bursts?
//
//   verify <capture-file> --seed N [--limit N] [--faults N] [--no-serve]
//
// The properties are the same two:
//
//   detection — the messages the client reports missing are exactly the ones that never
//               reached it, no more and no fewer;
//   repair    — with a retransmit server, nothing is missing at the end and every message
//               was delivered exactly once.
//
// Exit code is zero only if both hold. A tool that printed a discrepancy and exited zero
// would be the same mistake as a receiver that publishes a book it knows is wrong.

#include "support/capture_source.hpp"
#include "support/verify_pipeline.hpp"

#include <dfr/capture/ethernet.hpp>
#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/schedule.hpp>
#include <dfr/chaos/target.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/rng.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cap = dfr::capture;
namespace chaos = dfr::chaos;
namespace iex = dfr::wire::iextp;
namespace rec = dfr::recovery;

using dfr_tools::at_us;
using dfr_tools::derivable_faults;
using dfr_tools::drain;
using dfr_tools::offer_if_intact;
using dfr_tools::retransmit_server;
using dfr_tools::source_packet;
using dfr_tools::tally;
using dfr_tools::verify_client;
using dfr_tools::verify_options;

namespace {

struct options {
  const char* path{nullptr};
  std::uint64_t seed{1};
  std::uint64_t limit{0};
  std::uint32_t faults{6};
  bool serve{true};
};

bool parse(int argc, char** argv, options& into) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: verify <capture-file> [--seed N] [--limit N] "
                 "[--faults N] [--no-serve]\n");
    return false;
  }
  into.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      into.seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
      into.limit = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--faults") == 0 && i + 1 < argc) {
      into.faults =
          static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--no-serve") == 0) {
      into.serve = false;
    } else {
      std::fprintf(stderr, "verify: unrecognised argument %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

// Collects the IEX-TP packets from a capture, in order, as the injector's source stream.
std::vector<source_packet> collect_stream(dfr::packet_view file,
                                          std::uint64_t limit,
                                          dfr_tools::read_outcome& outcome,
                                          bool& ok) {
  std::vector<source_packet> stream;
  ok = dfr_tools::walk_capture(
      file, "verify", limit, outcome, [&](const cap::frame& captured) {
        cap::udp_datagram datagram;
        if (cap::parse_udp(captured).get(datagram) != dfr::error::ok) {
          return;
        }
        iex::header header;
        if (iex::decode_header(datagram.payload).get(header) != dfr::error::ok) {
          return;
        }
        if (!iex::verify_payload_framing(datagram.payload)) {
          return;
        }
        stream.push_back(source_packet{
            .bytes = std::string{reinterpret_cast<const char*>(
                                     datagram.payload.data()),
                                 datagram.payload.size()},
            .first_sequence = header.first_sequence,
            .message_count = header.message_count});
      });
  return stream;
}

}  // namespace

int main(int argc, char** argv) {
  options opts;
  if (!parse(argc, argv, opts)) {
    return 2;
  }

  bool ok = false;
  const std::string contents =
      dfr_tools::read_whole_file(opts.path, "verify", ok);
  if (!ok) {
    return 1;
  }

  dfr_tools::read_outcome outcome;
  const std::vector<source_packet> stream = collect_stream(
      dfr::packet_view{contents.data(), contents.size()}, opts.limit, outcome, ok);
  if (!ok) {
    return 1;
  }

  std::printf("capture %s\n", opts.path);
  std::printf("  format                   %s\n",
              outcome.is_pcapng ? "pcapng" : "classic pcap");
  std::printf("  frames read              %llu\n",
              static_cast<unsigned long long>(outcome.frames));
  std::printf("  IEX-TP packets usable    %zu\n", stream.size());
  if (stream.empty()) {
    std::fprintf(stderr, "verify: no usable IEX-TP packets in %s\n", opts.path);
    return 1;
  }

  chaos::schedule plan;
  dfr::prng rng{opts.seed};
  chaos::schedule_options schedule_opts;
  schedule_opts.max_faults = opts.faults;
  schedule_opts.permitted = derivable_faults();
  if (chaos::schedule::generate(rng, schedule_opts, stream.size()).get(plan) !=
      dfr::error::ok) {
    std::fprintf(stderr, "verify: could not build a schedule\n");
    return 1;
  }

  chaos::injector<chaos::iextp_target> injector{plan};
  verify_client client{verify_options()};
  const retransmit_server server{stream};
  tally record;
  std::int64_t clock_us = 0;

  const auto emit = [&](const chaos::emission& e) {
    clock_us += 20;
    offer_if_intact(client, record, e.packet, at_us(clock_us));
    if (opts.serve) {
      drain(client, record, server, clock_us);
    }
  };

  for (std::uint64_t i = 0; i < stream.size(); ++i) {
    const dfr::packet_view view{stream[i].bytes.data(), stream[i].bytes.size()};
    if (!injector.offer(view, i, emit)) {
      std::fprintf(stderr, "verify: injector refused packet %llu\n",
                   static_cast<unsigned long long>(i));
      return 1;
    }
  }
  if (!injector.flush(emit)) {
    std::fprintf(stderr, "verify: injector could not flush\n");
    return 1;
  }
  if (opts.serve) {
    // A hole revealed by the very last packet is requested and never served unless the loop
    // keeps running: the request is not due until the settle delay has passed, and there are
    // no further packets to prompt another poll.
    for (int round = 0; round < 64; ++round) {
      const auto before = record.retransmits_served;
      clock_us += 200;
      drain(client, record, server, clock_us);
      if (record.retransmits_served == before) {
        break;
      }
    }
  }

  // The two properties, computed from what happened rather than from what was scheduled.
  // The tracker's expectation rather than the delivered watermark: a heartbeat announces the
  // next message's sequence without delivering anything, so a hole can legitimately sit above
  // what has been delivered, and stopping the accounting at the delivered prefix would leave
  // that hole unchecked.
  const std::uint64_t high =
      client.tracking().expected_sequence(rec::channel_id::at(0));
  const std::uint64_t low =
      record.deliveries.empty() ? 0 : record.deliveries.begin()->first;

  std::set<std::uint64_t> reported_missing;
  const auto& holes = client.tracking().outstanding(rec::channel_id::at(0));
  for (const auto& hole : holes.ranges()) {
    for (std::uint64_t s = hole.first; s < hole.end; ++s) {
      reported_missing.insert(s);
    }
  }
  std::set<std::uint64_t> never_offered;
  for (std::uint64_t s = low; s < high; ++s) {
    if (!record.offered.contains(s)) {
      never_offered.insert(s);
    }
  }
  std::uint64_t unaccounted = 0;
  for (std::uint64_t s = low; s < high; ++s) {
    const auto delivered = record.deliveries.find(s);
    const bool once =
        delivered != record.deliveries.end() && delivered->second == 1;
    if (once == reported_missing.contains(s)) {
      ++unaccounted;  // both, or neither
    }
  }

  const auto& injected = injector.stats();
  std::printf("injected (seed %llu)\n",
              static_cast<unsigned long long>(opts.seed));
  std::printf("  faults scheduled         %zu\n", plan.faults().size());
  std::printf("  packets dropped          %llu\n",
              static_cast<unsigned long long>(injected.dropped));
  std::printf("  packets duplicated       %llu\n",
              static_cast<unsigned long long>(injected.duplicated));
  std::printf("  packets delayed          %llu\n",
              static_cast<unsigned long long>(injected.delayed));
  std::printf("  packets corrupted        %llu\n",
              static_cast<unsigned long long>(injected.mutated));
  std::printf("  faults not applicable    %llu\n",
              static_cast<unsigned long long>(injected.not_applicable));
  std::printf("client\n");
  std::printf("  state                    %s\n",
              rec::name_of(client.state()).data());
  std::printf("  packets accepted         %llu\n",
              static_cast<unsigned long long>(record.offered_packets));
  std::printf("  packets discarded        %llu\n",
              static_cast<unsigned long long>(record.discarded_packets));
  std::printf("  messages delivered       %zu\n", record.deliveries.size());
  std::printf("  retransmits served       %llu\n",
              static_cast<unsigned long long>(record.retransmits_served));
  std::printf("  gaps opened              %llu\n",
              static_cast<unsigned long long>(
                  client.tracking().stats(rec::channel_id::at(0)).gaps_opened));
  std::printf("  requests sent            %llu\n",
              static_cast<unsigned long long>(
                  client.retransmission().stats().requests_sent));
  std::printf("oracle\n");
  std::printf("  span checked             %llu..%llu\n",
              static_cast<unsigned long long>(low),
              static_cast<unsigned long long>(high));
  std::printf("  reported missing         %zu\n", reported_missing.size());
  std::printf("  actually never arrived   %zu\n", never_offered.size());
  std::printf("  delivered twice          %llu\n",
              static_cast<unsigned long long>(record.double_deliveries));
  std::printf("  unaccounted sequences    %llu\n",
              static_cast<unsigned long long>(unaccounted));

  const bool detection_holds = reported_missing == never_offered;
  const bool no_duplicates = record.double_deliveries == 0;
  const bool balanced = unaccounted == 0;
  const bool repaired = !opts.serve || reported_missing.empty();

  std::printf("verdict\n");
  std::printf("  detection exact          %s\n", detection_holds ? "yes" : "NO");
  std::printf("  every message once       %s\n", no_duplicates ? "yes" : "NO");
  std::printf("  accounting balances      %s\n", balanced ? "yes" : "NO");
  if (opts.serve) {
    std::printf("  fully repaired           %s\n", repaired ? "yes" : "NO");
  }
  if (!outcome.clean()) {
    std::printf("  capture read             stopped: %s, %zu bytes unconsumed\n",
                dfr::to_string(outcome.stopped).data(),
                outcome.unconsumed_bytes);
  }
  if (record.left_live) {
    std::printf("  client left the live path (state %s)\n",
                rec::name_of(client.state()).data());
  }

  const bool all = detection_holds && no_duplicates && balanced && repaired &&
                   outcome.clean() && !record.left_live;
  return all ? 0 : 1;
}

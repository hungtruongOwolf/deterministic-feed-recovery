// inspect — read a capture file and report what is in it.
//
// This is the tool that settles the open question in dfr/wire/iextp.hpp: the
// field offsets there were transcribed from a specification whose live URL now
// serves a stub, so they have a single source and have never been checked against
// real data. The check that settles it is a whole capture decoding with zero
// chain breaks across all three of IEX-TP's redundant chains.
//
// File I/O lives here rather than in the library, deliberately. A library that
// opens files cannot be driven from a fuzzer or a deterministic simulation, so
// the readers take bytes and this tool is the one place that produces them.
//
// Usage:  inspect <capture-file> [--limit N] [--quiet]

#include <dfr/capture/ethernet.hpp>
#include <dfr/capture/pcap.hpp>
#include <dfr/capture/pcapng.hpp>
#include <dfr/wire/iextp.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cap = dfr::capture;
namespace iex = dfr::wire::iextp;

namespace {

struct counters {
  std::uint64_t frames = 0;
  std::uint64_t truncated_frames = 0;
  std::uint64_t udp_datagrams = 0;
  std::uint64_t non_ip_frames = 0;
  std::uint64_t iextp_packets = 0;
  std::uint64_t iextp_heartbeats = 0;
  std::uint64_t messages = 0;
  std::uint64_t framing_verified = 0;

  std::set<std::uint16_t> vlans;
  std::set<std::uint32_t> groups;
  std::set<std::uint16_t> ports;
  std::set<std::uint32_t> channels;
  std::set<std::uint32_t> sessions;

  // Every failure, by code, with the frame index of the first occurrence.
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> failures;

  // Set when the capture itself stopped before the end, which is distinct from a
  // decode failure inside a frame.
  dfr::error read_stopped = dfr::error::ok;
  std::uint64_t unconsumed_bytes = 0;

  void note(dfr::error err, std::uint64_t at) {
    auto& entry = failures[std::string{dfr::to_string(err)}];
    if (entry.first == 0) {
      entry.second = at;
    }
    ++entry.first;
  }
};

std::string read_whole_file(const char* path, bool& ok) {
  ok = false;
  std::FILE* handle = std::fopen(path, "rb");
  if (handle == nullptr) {
    std::fprintf(stderr, "inspect: cannot open %s\n", path);
    return {};
  }
  std::string out;
  char buffer[1 << 16];
  while (const std::size_t got = std::fread(buffer, 1, sizeof buffer, handle)) {
    out.append(buffer, got);
  }
  const bool failed = std::ferror(handle) != 0;
  std::fclose(handle);
  if (failed) {
    std::fprintf(stderr, "inspect: read error on %s\n", path);
    return {};
  }
  ok = true;
  return out;
}

std::string ipv4_text(std::uint32_t address) {
  char text[16];
  std::snprintf(text, sizeof text, "%u.%u.%u.%u", (address >> 24) & 0xFF,
                (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
  return text;
}

// Everything above the link layer: demultiplex, decode, and check the chains.
//
// One chain_checker per channel, because IEX-TP's chains are per channel and
// feeding two channels into one checker would report a gap on every alternation.
class analyser {
 public:
  void observe(const cap::frame& captured) {
    ++counts_.frames;
    if (captured.truncated()) {
      ++counts_.truncated_frames;
    }

    cap::udp_datagram datagram;
    if (const auto err = cap::parse_udp(captured).get(datagram);
        err != dfr::error::ok) {
      if (err == dfr::error::not_supported) {
        ++counts_.non_ip_frames;  // ARP, IPv6, LLDP: expected in a real capture
      } else {
        counts_.note(err, counts_.frames);
      }
      return;
    }

    ++counts_.udp_datagrams;
    counts_.vlans.insert(datagram.vlan_id);
    counts_.groups.insert(datagram.destination_address);
    counts_.ports.insert(datagram.destination_port);

    iex::header header;
    if (const auto err = iex::decode_header(datagram.payload).get(header);
        err != dfr::error::ok) {
      counts_.note(err, counts_.frames);
      return;
    }

    ++counts_.iextp_packets;
    counts_.channels.insert(header.channel);
    counts_.sessions.insert(header.session);
    if (header.kind() == iex::packet_kind::heartbeat) {
      ++counts_.iextp_heartbeats;
    }

    // Chain three: the block framing must account for exactly the declared
    // payload length. Checked per packet.
    if (const auto framing = iex::verify_payload_framing(datagram.payload);
        !framing) {
      counts_.note(framing.error_code(), counts_.frames);
    } else {
      ++counts_.framing_verified;
      iex::message_cursor cursor;
      if (iex::message_cursor::over(datagram.payload).get(cursor) ==
          dfr::error::ok) {
        counts_.messages += cursor.remaining();
      }
    }

    // Chains one and two: sequence numbers and stream offsets, across packets.
    if (const auto chained = checkers_[header.channel].observe(header);
        !chained) {
      counts_.note(chained.error_code(), counts_.frames);
    }
  }

  [[nodiscard]] const counters& counts() const { return counts_; }

  void record_read_stop(dfr::error err, std::size_t unconsumed) {
    counts_.read_stopped = err;
    counts_.unconsumed_bytes = unconsumed;
  }

 private:
  counters counts_;
  std::map<std::uint32_t, iex::chain_checker> checkers_;
};

template <typename Reader>
void run(Reader& reader, analyser& into, std::uint64_t limit) {
  while (!reader.done()) {
    if (limit != 0 && into.counts().frames >= limit) {
      break;
    }
    cap::frame captured;
    const auto err = reader.next().get(captured);
    if (err == dfr::error::end_of_session) {
      break;  // pcapng: trailing metadata only
    }
    if (err != dfr::error::ok) {
      into.record_read_stop(err, reader.remaining());
      return;
    }
    into.observe(captured);
  }
}

void report(const counters& c, bool quiet) {
  const auto line = [](const char* label, std::uint64_t value) {
    std::printf("  %-24s %llu\n", label, static_cast<unsigned long long>(value));
  };

  std::printf("frames\n");
  line("total", c.frames);
  line("truncated by snaplen", c.truncated_frames);
  line("non-IP (ARP/IPv6/...)", c.non_ip_frames);
  line("UDP datagrams", c.udp_datagrams);

  std::printf("IEX-TP\n");
  line("packets", c.iextp_packets);
  line("heartbeats", c.iextp_heartbeats);
  line("payload framing verified", c.framing_verified);
  line("messages", c.messages);

  if (!quiet) {
    std::printf("observed\n");
    std::printf("  VLANs                   ");
    for (const std::uint16_t v : c.vlans) {
      std::printf("%u ", v);
    }
    std::printf("\n  multicast groups        ");
    for (const std::uint32_t g : c.groups) {
      std::printf("%s ", ipv4_text(g).c_str());
    }
    std::printf("\n  destination ports       ");
    for (const std::uint16_t p : c.ports) {
      std::printf("%u ", p);
    }
    std::printf("\n  channels                ");
    for (const std::uint32_t ch : c.channels) {
      std::printf("%u ", ch);
    }
    std::printf("\n  sessions                ");
    for (const std::uint32_t s : c.sessions) {
      std::printf("%u ", s);
    }
    std::printf("\n");
  }

  std::printf("capture read\n");
  if (c.read_stopped == dfr::error::ok) {
    std::printf("  reached the end of the file cleanly\n");
  } else {
    std::printf("  STOPPED EARLY: %s, %llu bytes unconsumed\n",
                dfr::to_string(c.read_stopped).data(),
                static_cast<unsigned long long>(c.unconsumed_bytes));
  }

  std::printf("chain breaks and decode errors\n");
  if (c.failures.empty()) {
    std::printf("  none\n");
  } else {
    for (const auto& [name, occurrence] : c.failures) {
      std::printf("  %-24s %llu (first at frame %llu)\n", name.c_str(),
                  static_cast<unsigned long long>(occurrence.first),
                  static_cast<unsigned long long>(occurrence.second));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: inspect <capture-file> [--limit N] [--quiet]\n");
    return 2;
  }

  std::uint64_t limit = 0;
  bool quiet = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
      limit = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--quiet") == 0) {
      quiet = true;
    }
  }

  bool ok = false;
  const std::string contents = read_whole_file(argv[1], ok);
  if (!ok) {
    return 1;
  }
  const dfr::packet_view file{contents.data(), contents.size()};
  std::printf("file %s (%llu bytes)\n", argv[1],
              static_cast<unsigned long long>(contents.size()));

  analyser into;

  // Try classic pcap first; it reports not_supported on a pcapng magic, which is
  // exactly what makes this fallback possible. IEX HIST switched format on
  // 2017-06-20, so a tool covering the corpus must handle both.
  cap::pcap::reader classic;
  if (const auto err = cap::pcap::reader::over(file).get(classic);
      err == dfr::error::ok) {
    std::printf("format classic pcap, %s-endian, %s timestamps, snaplen %u, "
                "link %u\n",
                classic.info().order == cap::pcap::byte_order::little ? "little"
                                                                     : "big",
                classic.info().resolution ==
                        cap::pcap::timestamp_resolution::microseconds
                    ? "microsecond"
                    : "nanosecond",
                classic.info().snaplen, classic.info().link);
    run(classic, into, limit);
  } else {
    cap::pcapng::reader modern;
    if (const auto ng_err = cap::pcapng::reader::over(file).get(modern);
        ng_err != dfr::error::ok) {
      std::fprintf(stderr,
                   "inspect: not a capture file (pcap said %s, pcapng said %s)\n",
                   dfr::to_string(err).data(), dfr::to_string(ng_err).data());
      return 1;
    }
    std::printf("format pcapng\n");
    run(modern, into, limit);
    std::printf("  sections %u, interfaces %zu, blocks skipped %llu\n",
                modern.sections(), modern.interfaces(),
                static_cast<unsigned long long>(modern.blocks_skipped()));
  }

  report(into.counts(), quiet);

  // Non-zero exit when anything failed, including a capture that stopped before
  // the end. Reporting a partial read as success is exactly the mistake the
  // readers are built to prevent, so the tool must not make it either.
  const bool clean = into.counts().failures.empty() &&
                     into.counts().read_stopped == dfr::error::ok;
  return clean ? 0 : 1;
}

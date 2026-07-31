// Reading a capture file and walking its frames, for the tools that need it.
//
// Two tools now do this — inspect and verify — and the format handling is the part with a
// real finding behind it, so it lives in one place rather than being copied. From the
// README: there is no format switch to find. `20170826` is classic pcap, `20170923` is
// pcapng, and `20241001` is classic pcap again seven years later with a different snaplen.
// A tool cannot pick a reader by date or by era; it has to try one and fall back.
//
// File I/O stays in tools/ and out of the library, per docs/DESIGN.md. The library takes
// bytes it does not own.

#ifndef DFR_TOOLS_SUPPORT_CAPTURE_SOURCE_HPP
#define DFR_TOOLS_SUPPORT_CAPTURE_SOURCE_HPP

#include <dfr/capture/pcap.hpp>
#include <dfr/capture/pcapng.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>

#include <cstdio>
#include <string>

namespace dfr_tools {

namespace cap = dfr::capture;

// How a walk ended, so a partial read can reach the exit code.
//
// Reporting a truncated capture as success is exactly the mistake the readers were built to
// prevent, so a tool must not make it either — which means the outcome has to be a value
// the caller cannot ignore rather than something printed and forgotten.
struct read_outcome {
  dfr::error stopped{dfr::error::ok};
  std::size_t unconsumed_bytes{0};
  std::uint64_t frames{0};
  bool is_pcapng{false};

  [[nodiscard]] bool clean() const { return stopped == dfr::error::ok; }
};

inline std::string read_whole_file(const char* path, const char* tool, bool& ok) {
  ok = false;
  std::FILE* handle = std::fopen(path, "rb");
  if (handle == nullptr) {
    std::fprintf(stderr, "%s: cannot open %s\n", tool, path);
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
    std::fprintf(stderr, "%s: read error on %s\n", tool, path);
    return {};
  }
  ok = true;
  return out;
}

template <typename Reader, typename Handler>
void walk(Reader& reader, read_outcome& outcome, std::uint64_t limit,
          Handler&& handler) {
  while (!reader.done()) {
    if (limit != 0 && outcome.frames >= limit) {
      return;
    }
    cap::frame captured;
    const auto err = reader.next().get(captured);
    if (err == dfr::error::end_of_session) {
      return;  // pcapng: trailing metadata only
    }
    if (err != dfr::error::ok) {
      outcome.stopped = err;
      outcome.unconsumed_bytes = reader.remaining();
      return;
    }
    ++outcome.frames;
    handler(captured);
  }
}

// Tries classic pcap, then pcapng. Classic reports not_supported on a pcapng magic, which
// is what makes the fallback possible.
//
// Returns false only when the file is neither format; a file that *is* a capture but stops
// early returns true with `stopped` set, because those are different problems and a caller
// wants to report the frames it did read.
template <typename Handler>
bool walk_capture(dfr::packet_view file, const char* tool, std::uint64_t limit,
                  read_outcome& outcome, Handler&& handler) {
  cap::pcap::reader classic;
  const auto classic_err = cap::pcap::reader::over(file).get(classic);
  if (classic_err == dfr::error::ok) {
    walk(classic, outcome, limit, handler);
    return true;
  }

  cap::pcapng::reader modern;
  const auto ng_err = cap::pcapng::reader::over(file).get(modern);
  if (ng_err != dfr::error::ok) {
    std::fprintf(stderr, "%s: not a capture file (pcap said %s, pcapng said %s)\n",
                 tool, dfr::to_string(classic_err).data(),
                 dfr::to_string(ng_err).data());
    return false;
  }
  outcome.is_pcapng = true;
  walk(modern, outcome, limit, handler);
  return true;
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_CAPTURE_SOURCE_HPP

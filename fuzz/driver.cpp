// A fuzzing driver that works where libFuzzer does not, and finds the same bugs from a seed.
//
// libFuzzer's runtime is not shipped with every toolchain — it is absent from the Xcode clang this project is
// developed on — and "we fuzz in CI" is a poor answer when the person writing the decoder cannot run it. So the
// targets are plain `LLVMFuzzerTestOneInput` functions and there are two ways to drive them:
//
//   * real libFuzzer, coverage-guided, in CI on Linux, where it explores far better than anything here;
//   * this, everywhere, seeded and therefore reproducible.
//
// The second is not a poor imitation of the first, it is a different tool. Coverage-guided fuzzing finds more,
// and finds it at inputs nobody can reconstruct without the corpus file. This finds less, and every finding is
// a seed plus an index — which is the same property the rest of the project is built on, and it means a failure
// can be handed to somebody as two numbers rather than as an attachment.
//
// The mutations are deliberately crude: bit flips, byte splices, length truncations, and the interesting values
// for a length field. A decoder's hostile input is almost never structurally novel — it is a valid packet with a
// wrong length, which is exactly what these produce.

#include "checks.hpp"

#include <dfr/core/narrow.hpp>
#include <dfr/core/rng.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Defined by the target this driver is linked against.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return {};
  }
  std::fseek(f, 0, SEEK_END);
  const auto size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<std::uint8_t> out(size > 0 ? static_cast<std::size_t>(size) : 0);
  if (!out.empty() && std::fread(out.data(), 1, out.size(), f) != out.size()) {
    out.clear();
  }
  std::fclose(f);
  return out;
}

// The values that break length arithmetic, tried explicitly rather than waited for.
//
// A random mutator reaches 0xFFFF in a two-byte field eventually; putting it in the table reaches it on the
// first pass. These are the numbers that have historically broken framing in this repository: a length of zero,
// a length one past the buffer, and the unsigned maxima.
constexpr std::uint64_t kInterestingLengths[]{0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF,
                                              0x8000, 0xFFFF, 0xFFFFFFFF};

void mutate(std::vector<std::uint8_t>& bytes, dfr::prng& rng) {
  if (bytes.empty()) {
    bytes.push_back(static_cast<std::uint8_t>(rng.next() & 0xFF));
    return;
  }
  switch (rng.next() % 5) {
    case 0: {  // flip one bit
      const auto at = dfr::narrowed<std::size_t>(rng.next() % bytes.size());
      bytes[at] = static_cast<std::uint8_t>(bytes[at] ^ (1U << (rng.next() % 8)));
      return;
    }
    case 1: {  // overwrite one byte
      const auto at = dfr::narrowed<std::size_t>(rng.next() % bytes.size());
      bytes[at] = static_cast<std::uint8_t>(rng.next() & 0xFF);
      return;
    }
    case 2: {  // truncate — the single most productive mutation against a framer
      const auto keep = dfr::narrowed<std::size_t>(rng.next() % bytes.size());
      bytes.resize(keep);
      return;
    }
    case 3: {  // write an interesting length somewhere a length field might be
      const auto value =
          kInterestingLengths[rng.next() % (sizeof kInterestingLengths / sizeof(std::uint64_t))];
      const auto at = dfr::narrowed<std::size_t>(rng.next() % bytes.size());
      for (std::size_t i = 0; i < 4 && at + i < bytes.size(); ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
      }
      return;
    }
    default: {  // grow, so a decoder that only ever sees short input is not the only thing tested
      bytes.push_back(static_cast<std::uint8_t>(rng.next() & 0xFF));
      return;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t seed = 1;
  std::uint64_t rounds = 20'000;
  std::vector<const char*> corpus;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
      rounds = std::strtoull(argv[++i], nullptr, 10);
    } else {
      corpus.push_back(argv[i]);
    }
  }

  // Every corpus file, unmutated, first. A corpus that no longer parses is a regression on its own, and finding
  // that out after twenty thousand mutations would bury it.
  std::vector<std::vector<std::uint8_t>> seeds;
  for (const char* path : corpus) {
    auto bytes = read_file(path);
    if (bytes.empty()) {
      std::fprintf(stderr, "driver: could not read %s\n", path);
      return 1;
    }
    (void)LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    seeds.push_back(std::move(bytes));
  }
  if (seeds.empty()) {
    // No corpus: start from nothing, which is itself worth trying — an empty input is the case a decoder is
    // most likely to have been written without.
    seeds.emplace_back();
  }

  dfr::prng rng{seed};
  for (std::uint64_t round = 0; round < rounds; ++round) {
    auto bytes = seeds[dfr::narrowed<std::size_t>(rng.next()) % seeds.size()];
    // One to four mutations, so a single wrong byte and a thoroughly mangled packet are both reached.
    const auto steps = 1 + rng.next() % 4;
    for (std::uint64_t i = 0; i < steps; ++i) {
      mutate(bytes, rng);
    }
    (void)LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
  }

  std::printf("driver: %llu corpus inputs, %llu mutations from seed %llu — no invariant broken\n",
              static_cast<unsigned long long>(corpus.size()),
              static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(seed));
  return 0;
}

#include <dfr/core/attributes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

// The reason kCacheLineSize exists rather than using the standard constant
// directly is that padding to too small a value silently reintroduces false
// sharing. A test cannot detect false sharing, but it can pin the property the
// constant is chosen for: that two members padded by it land on distinct lines
// under the platform's real line size.
struct PaddedPair {
  alignas(dfr::kCacheLineSize) std::atomic<std::uint64_t> first{0};
  alignas(dfr::kCacheLineSize) std::atomic<std::uint64_t> second{0};
};

DFR_NOINLINE int noinline_identity(int x) { return x; }
DFR_FLATTEN_INLINE int flatten_identity(int x) { return x; }
DFR_COLD int cold_identity(int x) { return x; }

}  // namespace

TEST_CASE("cache line size is a usable power of two", "[core][attributes]") {
  STATIC_REQUIRE(dfr::kCacheLineSize >= 64);
  STATIC_REQUIRE((dfr::kCacheLineSize & (dfr::kCacheLineSize - 1)) == 0);

  // On Apple Silicon we deliberately over-pad to 128 even though libc++
  // reports 64, because the M-series L2 line is 128 bytes. Assert the
  // divergence rather than leaving it as a comment, so that a future
  // libc++ that reports 128 does not make the override silently redundant.
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  STATIC_REQUIRE(dfr::kCacheLineSize >= 128);
#endif
}

TEST_CASE("cache-line padding separates two atomics", "[core][attributes]") {
  PaddedPair pair;

  const auto first = reinterpret_cast<std::uintptr_t>(&pair.first);
  const auto second = reinterpret_cast<std::uintptr_t>(&pair.second);

  REQUIRE(second > first);
  CHECK(second - first >= dfr::kCacheLineSize);
  CHECK(first / dfr::kCacheLineSize != second / dfr::kCacheLineSize);
}

TEST_CASE("inlining attributes compile and preserve semantics",
          "[core][attributes]") {
  // The value of these is in the generated code, which a unit test cannot
  // observe. What it can do is fail the build if an attribute is spelled in a
  // position the compiler rejects, which is the mistake that actually happens
  // when porting the macros to a new compiler.
  CHECK(noinline_identity(7) == 7);
  CHECK(flatten_identity(7) == 7);
  CHECK(cold_identity(7) == 7);
}

TEST_CASE("branch hint attributes compile in both arms",
          "[core][attributes]") {
  int taken = 0;
  for (int i = 0; i < 4; ++i) {
    if (i == 0) {
      DFR_UNLIKELY taken += 1;
    } else {
      DFR_LIKELY taken += 10;
    }
  }
  CHECK(taken == 31);
}

TEST_CASE("the inline namespace is transparent", "[core][attributes]") {
  // dfr::kCacheLineSize and dfr::v1::kCacheLineSize must name the same entity,
  // otherwise the ABI versioning scheme has leaked into the public spelling.
  STATIC_REQUIRE(&dfr::kCacheLineSize == &dfr::v1::kCacheLineSize);
}

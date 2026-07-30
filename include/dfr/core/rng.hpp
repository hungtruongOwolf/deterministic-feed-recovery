// Seeded randomness.
//
// Every random decision in this library comes from an explicitly passed prng.
// There is no global generator, and there is no way to obtain a random *value*
// without one — the only nondeterministic thing available is a seed, and it can
// only be fetched at start-up. FoundationDB achieves the same discipline with
// two differently named globals, deterministicRandom() and
// nondeterministicRandom(), so that misuse is greppable; making the
// nondeterministic half return a seed rather than a stream is stronger, because
// it cannot be called mid-simulation at all.
//
// Three deliberate choices, each closing a determinism leak by construction
// rather than by discipline (BUILD-GUIDE.md section 11.6):
//
//   1. Our own engine, not std::mt19937_64. The *engine* is portable — the
//      standard specifies its algorithm — so that was not the reason. The
//      reasons are state size and speed: mt19937_64 carries 2,504 bytes of
//      state, which has to be copied every time a simulation is checkpointed
//      for a determinism self-check, while xoshiro256++ carries 32 and is
//      several times faster per draw. Over millions of draws per seed, both
//      matter.
//
//   2. Our own range reduction, never std::uniform_int_distribution. Leak #4:
//      [rand.dist.general] mandates no particular algorithm, so the same seed
//      produces different values on libstdc++ and libc++. A recorded seed would
//      then replay differently on another machine, which destroys the entire
//      premise.
//
//   3. No floating point anywhere in the API. Leaks #11 and #12 — FP
//      contraction and libm version drift — cannot affect what this class
//      produces if it never computes with a double. Probabilities are integer
//      ratios, which is TigerBeetle's stdx.PRNG design.

#ifndef DFR_CORE_RNG_HPP
#define DFR_CORE_RNG_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>

namespace dfr::inline v1 {

namespace detail {

// The full 128-bit product of two 64-bit values.
struct wide_product {
  std::uint64_t high;
  std::uint64_t low;

  [[nodiscard]] friend constexpr bool operator==(wide_product,
                                                 wide_product) = default;
};

// Schoolbook 32x32 decomposition. Always compiled, even where a 128-bit type is
// available, for one reason: it is the path MSVC takes, a wrong version of it
// would silently bias every bounded draw, and nothing else would ever execute
// it. Compiling it unconditionally lets a test compare the two implementations
// on whatever platform the test happens to run on.
[[nodiscard]] constexpr wide_product wide_multiply_portable(
    std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a_lo = a & 0xFFFF'FFFFULL;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & 0xFFFF'FFFFULL;
  const std::uint64_t b_hi = b >> 32;

  const std::uint64_t ll = a_lo * b_lo;
  const std::uint64_t lh = a_lo * b_hi;
  const std::uint64_t hl = a_hi * b_lo;
  const std::uint64_t hh = a_hi * b_hi;

  const std::uint64_t mid =
      (ll >> 32) + (lh & 0xFFFF'FFFFULL) + (hl & 0xFFFF'FFFFULL);
  const std::uint64_t low = (ll & 0xFFFF'FFFFULL) | (mid << 32);
  const std::uint64_t high = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
  return wide_product{high, low};
}

[[nodiscard]] DFR_FLATTEN_INLINE constexpr wide_product wide_multiply(
    std::uint64_t a, std::uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
  // __extension__ silences -Wpedantic: there is no standard 128-bit type in
  // C++20 and unsigned __int128 is a GNU extension.
  __extension__ using u128 = unsigned __int128;
  const u128 product = static_cast<u128>(a) * static_cast<u128>(b);
  return wide_product{static_cast<std::uint64_t>(product >> 64),
                      static_cast<std::uint64_t>(product)};
#else
  return wide_multiply_portable(a, b);
#endif
}

}  // namespace detail

// A probability, as an exact integer ratio. `ratio{1, 100}` is one per cent.
//
// Not a double, and not a percentage. A double would reintroduce the floating
// point leaks this class exists to avoid, and a percentage cannot express the
// small probabilities a fault injector needs — FoundationDB's BUGGIFY sites
// fire at rates that matter down to fractions of a per cent.
struct ratio {
  std::uint64_t numerator{0};
  std::uint64_t denominator{1};

  [[nodiscard]] friend constexpr bool operator==(ratio, ratio) = default;
};

[[nodiscard]] constexpr ratio percent(std::uint64_t n) noexcept {
  return ratio{n, 100};
}

inline constexpr ratio kNever{0, 1};
inline constexpr ratio kAlways{1, 1};

// ---------------------------------------------------------------------------
// prng
// ---------------------------------------------------------------------------

// xoshiro256++ 1.0, by David Blackman and Sebastiano Vigna (public domain,
// prng.di.unimi.it). Chosen for a 32-byte state and a handful of instructions
// per draw. Passes BigCrush; not cryptographic, which nothing here needs.
class prng {
 public:
  // The engine state, exposed so that a simulation can be checkpointed and
  // resumed, and so that two runs can be compared at a point rather than only
  // at the end.
  struct state {
    std::uint64_t s[4]{};
    std::uint64_t draws{0};

    [[nodiscard]] friend constexpr bool operator==(const state&,
                                                   const state&) = default;
  };

  // Any 64-bit value is a valid seed, including zero: the SplitMix64 expansion
  // below turns it into a full state with no all-zero possibility.
  explicit constexpr prng(std::uint64_t seed) noexcept {
    // SplitMix64, the seeding routine xoshiro's authors specify. Using the raw
    // seed as the state directly would make seed 0 produce an all-zero state,
    // from which xoshiro can never escape.
    std::uint64_t z = seed;
    for (std::uint64_t& word : state_.s) {
      z += 0x9E37'79B9'7F4A'7C15ULL;
      std::uint64_t x = z;
      x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
      x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EBULL;
      word = x ^ (x >> 31);
    }
    DFR_ASSERT(state_.s[0] != 0 || state_.s[1] != 0 || state_.s[2] != 0 ||
                   state_.s[3] != 0,
               "SplitMix64 must not produce an all-zero xoshiro state");
  }

  explicit constexpr prng(state saved) noexcept : state_(saved) {}

  // ---- raw output -------------------------------------------------------

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr std::uint64_t next() noexcept {
    const std::uint64_t result =
        std::rotl(state_.s[0] + state_.s[3], 23) + state_.s[0];

    const std::uint64_t t = state_.s[1] << 17;
    state_.s[2] ^= state_.s[0];
    state_.s[3] ^= state_.s[1];
    state_.s[1] ^= state_.s[2];
    state_.s[0] ^= state_.s[3];
    state_.s[2] ^= t;
    state_.s[3] = std::rotl(state_.s[3], 45);

    ++state_.draws;
    return result;
  }

  // ---- bounded draws ----------------------------------------------------

  // Uniform in [0, bound). Unbiased.
  //
  // Lemire's multiply-shift with rejection, not modulo. Modulo is what
  // FoundationDB does (`gen64() % range`) and it is biased: for a bound that
  // does not divide 2^64, the low residues are very slightly more likely. The
  // bias is invisible in ordinary use and matters here for one specific reason
  // — a fault injector's job is to explore the tails of a distribution, and a
  // generator that under-samples part of its range under-samples exactly the
  // schedules we are hunting for.
  //
  // The rejection loop is bounded in expectation, not absolutely, but it is
  // *deterministic*: the same seed rejects at the same points, so a replay is
  // exact. It is also why draws() can exceed the number of calls made.
  [[nodiscard]] constexpr std::uint64_t below(std::uint64_t bound) noexcept {
    DFR_ASSERT(bound > 0, "below(0) has no valid result; guard the call");
    if (bound == 1) {
      // No randomness needed, and deliberately consuming none: a caller that
      // narrows a range to one element during shrinking should not shift every
      // subsequent draw in the stream.
      return 0;
    }

    std::uint64_t value = next();
    auto [high, low] = detail::wide_multiply(value, bound);

    if (low < bound) {
      const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
      while (low < threshold) {
        value = next();
        const auto product = detail::wide_multiply(value, bound);
        high = product.high;
        low = product.low;
      }
    }
    return high;
  }

  // Uniform in [low, high], inclusive on both ends.
  //
  // Inclusive because every bound this library cares about is inclusive:
  // sequence numbers, byte offsets, block counts. An exclusive upper bound
  // invites the off-by-one TIGER_STYLE warns about between an index, a count
  // and a size.
  [[nodiscard]] constexpr std::uint64_t between(std::uint64_t low,
                                                std::uint64_t high) noexcept {
    DFR_ASSERT(low <= high, "between() requires low <= high");
    if (low == 0 && high == UINT64_MAX) {
      return next();  // the full range needs no reduction
    }
    return low + below(high - low + 1);
  }

  // A valid index into a container of `count` elements.
  [[nodiscard]] constexpr std::size_t index(std::size_t count) noexcept {
    DFR_ASSERT(count > 0, "index() into an empty container has no result");
    return static_cast<std::size_t>(below(static_cast<std::uint64_t>(count)));
  }

  // True with probability `p`.
  //
  // Integer arithmetic throughout. `chance(kNever)` and `chance(kAlways)`
  // consume nothing from the stream, so a fault site configured off does not
  // perturb the sequence a recorded seed replays — the property that lets a
  // fault schedule be reduced without invalidating everything after it.
  [[nodiscard]] constexpr bool chance(ratio p) noexcept {
    DFR_ASSERT(p.denominator > 0, "a ratio with a zero denominator is undefined");
    DFR_ASSERT(p.numerator <= p.denominator,
               "a probability above one is a programmer error");
    if (p.numerator == 0) {
      return false;
    }
    if (p.numerator == p.denominator) {
      return true;
    }
    return below(p.denominator) < p.numerator;
  }

  // A uniformly chosen element. Returns a reference so the caller can mutate it.
  template <typename T, std::size_t Extent>
  [[nodiscard]] constexpr T& pick(std::span<T, Extent> items) noexcept {
    DFR_ASSERT(!items.empty(), "pick() from an empty range has no result");
    return items[index(items.size())];
  }

  // Fisher-Yates, in place. Used to permute a fault schedule, and to permute
  // the order in which equally-due events are delivered — which is how a
  // simulator explores orderings a real network could produce.
  template <typename T, std::size_t Extent>
  constexpr void shuffle(std::span<T, Extent> items) noexcept {
    if (items.size() < 2) {
      return;
    }
    for (std::size_t i = items.size() - 1; i > 0; --i) {
      const std::size_t j = static_cast<std::size_t>(below(i + 1));
      if (i != j) {
        using std::swap;
        swap(items[i], items[j]);
      }
    }
  }

  // ---- state ------------------------------------------------------------

  [[nodiscard]] constexpr state save() const noexcept { return state_; }
  constexpr void restore(state saved) noexcept { state_ = saved; }

  // How many raw words have been drawn.
  //
  // This is the cheapest determinism self-check there is, and it is
  // FoundationDB's "unseed" idea in one integer: run a seed, record the count,
  // run it again, compare. A mismatch proves that something consumed randomness
  // a different number of times, which is the signature of a determinism leak,
  // long before the outputs visibly diverge.
  [[nodiscard]] constexpr std::uint64_t draws() const noexcept {
    return state_.draws;
  }

 private:
  state state_{};
};

// ---------------------------------------------------------------------------
// The only nondeterministic thing in the library
// ---------------------------------------------------------------------------

// A seed drawn from the platform, for "run this with a fresh seed".
//
// Declared in a header but defined out of line and deliberately noinline, so
// that a call to it is visible in a profile and a stack trace. Grep for this
// name to audit every place the library touches real entropy: there should be
// exactly one, in main().
//
// Context: process start-up only. Calling this from inside a simulation defeats
// the entire design, which is why it returns a seed rather than a value —
// there is no way to use it as a stream.
[[nodiscard]] DFR_NOINLINE inline std::uint64_t nondeterministic_seed() {
  // Two draws combined, because std::random_device::result_type is only
  // required to be unsigned int, which is 32 bits on every platform we target.
  // Seeding a 64-bit state from 32 bits would silently halve the seed space.
  std::random_device device;
  const auto high = static_cast<std::uint64_t>(device());
  const auto low = static_cast<std::uint64_t>(device());
  return (high << 32) | low;
}

}  // namespace dfr::inline v1

#endif  // DFR_CORE_RNG_HPP

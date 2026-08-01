// Compiler attributes and inlining control.
//
// Everything here is a portability shim. No dfr header should spell a
// compiler-specific attribute directly.

#ifndef DFR_CORE_ATTRIBUTES_HPP
#define DFR_CORE_ATTRIBUTES_HPP

#include <new>

// ---------------------------------------------------------------------------
// Feature detection
// ---------------------------------------------------------------------------

#if defined(__has_cpp_attribute)
#  define DFR_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
#  define DFR_HAS_CPP_ATTRIBUTE(x) 0
#endif

#if defined(__has_attribute)
#  define DFR_HAS_ATTRIBUTE(x) __has_attribute(x)
#else
#  define DFR_HAS_ATTRIBUTE(x) 0
#endif

#if defined(__has_builtin)
#  define DFR_HAS_BUILTIN(x) __has_builtin(x)
#else
#  define DFR_HAS_BUILTIN(x) 0
#endif

// ---------------------------------------------------------------------------
// Inlining
//
// Two levels, following simdjson's distinction. `DFR_INLINE` is a request the
// optimiser may decline. `DFR_FLATTEN_INLINE` overrides its judgement and is
// reserved for the few functions that must not appear in a call frame at all.
//
// The guard on optimisation level is simdjson's, and its reason is worth
// stating: forcing inlining in an unoptimised build produces significant code
// bloat and long compile times for no benefit, because nothing else is being
// optimised either.
// ---------------------------------------------------------------------------

#define DFR_INLINE inline

#if defined(__OPTIMIZE__) || defined(NDEBUG)
#  if defined(_MSC_VER) && !defined(__clang__)
#    define DFR_FLATTEN_INLINE __forceinline
#  elif DFR_HAS_ATTRIBUTE(always_inline)
#    define DFR_FLATTEN_INLINE inline __attribute__((always_inline))
#  else
#    define DFR_FLATTEN_INLINE inline
#  endif
#else
#  define DFR_FLATTEN_INLINE inline
#endif

// Keep a cold branch out of the caller's frame. Used on the slow paths that
// hang off a hot function, so the hot path keeps a minimal prologue. quill
// applies this to every bail-out in its logging front end.
#if defined(_MSC_VER) && !defined(__clang__)
#  define DFR_NOINLINE __declspec(noinline)
#elif DFR_HAS_ATTRIBUTE(noinline)
#  define DFR_NOINLINE __attribute__((noinline))
#else
#  define DFR_NOINLINE
#endif

// Hint that a whole function is rarely executed, so the optimiser may move it
// out of line and out of the hot instruction stream.
#if DFR_HAS_ATTRIBUTE(cold)
#  define DFR_COLD __attribute__((cold))
#else
#  define DFR_COLD
#endif

// ---------------------------------------------------------------------------
// Lifetime annotations
//
// These are the highest-value annotations in a zero-copy API, because every
// public dfr type that names a buffer is a non-owning view. Tagging views as
// pointers, owners as owners, and retained parameters as lifetime-bound lets
// the compiler diagnose the dangling-view mistake that the type system cannot
// express on its own.
//
// Note that these only ever produce warnings, and only on Clang and GCC.
// Abseil's guidance (Tip of the Week #149) is that the type system "is simply
// not capable of encoding the necessary details about lifespan requirements",
// so we use these *and* delete rvalue overloads where the mistake is obvious.
// ---------------------------------------------------------------------------

#if DFR_HAS_CPP_ATTRIBUTE(clang::lifetimebound)
#  define DFR_LIFETIME_BOUND [[clang::lifetimebound]]
#elif DFR_HAS_CPP_ATTRIBUTE(msvc::lifetimebound)
#  define DFR_LIFETIME_BOUND [[msvc::lifetimebound]]
#else
#  define DFR_LIFETIME_BOUND
#endif

// A non-owning view. Enables -Wdangling-gsl.
#if DFR_HAS_CPP_ATTRIBUTE(gsl::Pointer)
#  define DFR_VIEW [[gsl::Pointer]]
#else
#  define DFR_VIEW
#endif

// Owns the storage that a DFR_VIEW may refer to.
#if DFR_HAS_CPP_ATTRIBUTE(gsl::Owner)
#  define DFR_OWNER [[gsl::Owner]]
#else
#  define DFR_OWNER
#endif

// ---------------------------------------------------------------------------
// Branch hints
//
// Deliberately not wrapped in shorter names, and deliberately rare. Abseil's
// own header carries the warning that "annotating every branch in a codebase
// is likely counterproductive", because a wrong hint is worse than none: it
// moves the taken branch out of line.
//
// The rule for dfr: only on a branch whose bias is a documented protocol fact
// (a heartbeat is rarer than a data packet) or a checked precondition, and the
// comment must say which.
// ---------------------------------------------------------------------------

#if DFR_HAS_CPP_ATTRIBUTE(likely)
#  define DFR_LIKELY [[likely]]
#  define DFR_UNLIKELY [[unlikely]]
#else
#  define DFR_LIKELY
#  define DFR_UNLIKELY
#endif

// ---------------------------------------------------------------------------
// Unreachable
//
// Reaching this is a programmer error, so in a checked build it must trap
// rather than miscompile. dfr::detail::assert_unreachable in <dfr/core/assert.hpp>
// wraps this; prefer that over using this macro directly.
// ---------------------------------------------------------------------------

#if DFR_HAS_BUILTIN(__builtin_unreachable) || defined(__GNUC__)
#  define DFR_UNREACHABLE_INTRINSIC() __builtin_unreachable()
#elif defined(_MSC_VER)
#  define DFR_UNREACHABLE_INTRINSIC() __assume(0)
#else
#  define DFR_UNREACHABLE_INTRINSIC() ((void)0)
#endif

// ---------------------------------------------------------------------------
// ABI versioning
//
// Every public name lives in dfr::v1, exported through an inline namespace so
// that `dfr::` spelling continues to work while the mangled symbol carries the
// version. quill uses this to ship breaking changes without breaking links.
// ---------------------------------------------------------------------------

#define DFR_NAMESPACE_BEGIN                                                    \
  namespace dfr {                                                              \
  inline namespace v1 {
#define DFR_NAMESPACE_END                                                      \
  }                                                                            \
  }

namespace dfr::inline v1 {

// ---------------------------------------------------------------------------
// Cache line size
//
// std::hardware_destructive_interference_size is the standard answer and this
// header used to take it, widened on Apple arm64 because libc++ reports 64
// while the M-series L2 line is 128.
//
// It is no longer used at all, and GCC is the reason: it warns on any use of
// the constant (-Winterference-size), because **its value is part of the ABI**:
// two translation units built with different GCC versions can disagree about
// how wide a padded member is, and in a lock-free structure that is a layout
// mismatch rather than a performance question. A constant whose value depends
// on which compiler saw the header is not a property of the machine.
//
// So the sizes below are literals per architecture. That is what rigtorp's
// MPMCQueue does (MPMCQueue.h:43), it is what the comment above always argued
// for, and following the argument to its conclusion removes the dependency
// instead of documenting it.
//
// Over-padding costs bytes. Under-padding silently reintroduces false sharing,
// which is measured at 10-25% in docs/CONCURRENCY.md and is invisible in a
// profile. The asymmetry decides every case here.

#if defined(__aarch64__) || defined(__arm64__)
// 128 on every arm64 target, not only Apple's: the M-series L2 line is 128, and
// a Graviton or a Neoverse pads to 64 usefully but never harmfully at 128.
inline constexpr std::size_t kCacheLineSize = 128;
#else
// 64 on x86-64 and as the default. A target with a wider line pads too little
// here, so this is the one line to change when one appears.
inline constexpr std::size_t kCacheLineSize = 64;
#endif

static_assert(kCacheLineSize >= 64,
              "a cache line below 64 bytes is not a platform we support; "
              "padding computed from it would not separate two atomics");
static_assert((kCacheLineSize & (kCacheLineSize - 1)) == 0,
              "kCacheLineSize must be a power of two so that alignas and "
              "offset masking agree");

}  // namespace dfr::inline v1

#endif  // DFR_CORE_ATTRIBUTES_HPP

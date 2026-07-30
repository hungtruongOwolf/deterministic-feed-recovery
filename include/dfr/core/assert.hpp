// Assertions.
//
// The governing distinction, from TIGER_STYLE:
//
//   Assertions detect programmer errors. Unlike operating errors, which are
//   expected and which must be handled, assertion failures are unexpected. The
//   only correct way to handle corrupt code is to crash. Assertions downgrade
//   catastrophic correctness bugs into liveness bugs.
//
// So: malformed wire data is *not* an assertion. It is the product, and it is
// reported as a dfr::error value. Assertions are for our own mistakes.
//
// The level is set by the build (DFR_ASSERTIONS=off|fast|paranoid) and is
// deliberately independent of NDEBUG, so a release build can ship with
// assertions enabled. SQLite runs about three times slower with its assertions
// on and therefore ships with them off; we intend to publish our own measured
// figure rather than inherit that decision, which is why `off` exists as a
// level at all.

#ifndef DFR_CORE_ASSERT_HPP
#define DFR_CORE_ASSERT_HPP

#include <dfr/core/attributes.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

// ---------------------------------------------------------------------------
// Levels
//
// Numeric so that a header can compare them, and so that the build system only
// has to define one macro.
// ---------------------------------------------------------------------------

#define DFR_ASSERTION_LEVEL_OFF 0
#define DFR_ASSERTION_LEVEL_FAST 1
#define DFR_ASSERTION_LEVEL_PARANOID 2

#if !defined(DFR_ASSERTION_LEVEL)
// Chosen rather than defaulting to OFF so that a consumer who compiles these
// headers without our CMake still gets checked behaviour. Silently disabling
// every check because a macro was missing is the wrong failure mode.
#  define DFR_ASSERTION_LEVEL DFR_ASSERTION_LEVEL_FAST
#endif

#if DFR_ASSERTION_LEVEL < DFR_ASSERTION_LEVEL_OFF || \
    DFR_ASSERTION_LEVEL > DFR_ASSERTION_LEVEL_PARANOID
#  error "DFR_ASSERTION_LEVEL must be one of DFR_ASSERTION_LEVEL_{OFF,FAST,PARANOID}"
#endif

namespace dfr::inline v1 {

// Compile-time queries, so that callers can drop whole invariant helpers with
// `if constexpr` rather than wrapping them in preprocessor conditionals.
inline constexpr bool kAssertionsEnabled =
    DFR_ASSERTION_LEVEL >= DFR_ASSERTION_LEVEL_FAST;
inline constexpr bool kParanoidAssertionsEnabled =
    DFR_ASSERTION_LEVEL >= DFR_ASSERTION_LEVEL_PARANOID;

// ---------------------------------------------------------------------------
// The failure path
// ---------------------------------------------------------------------------

// Everything known about a failed assertion. Passed by const reference so that
// adding a field later is not an ABI break for a custom handler.
struct assert_context {
  std::string_view expression;  // the source text of the condition
  std::string_view message;     // the reason, or empty
  std::source_location where;
};

namespace detail {

// A handler must not return. Returning from a failed assertion continues
// executing code whose invariants are known to be broken, which is the
// behaviour assertions exist to prevent.
//
// It is a plain function pointer rather than a std::function because the
// failure path must not allocate: an assertion may well be firing *because*
// the allocator is in a bad state.
//
// Note on throwing handlers, which the test suite uses to observe a failure
// without ending the process: throwing satisfies "must not return", but only
// where the assertion sits in a function that permits an exception to escape.
// An assertion inside a `noexcept` function turns a throwing handler into
// std::terminate. That is the right runtime behaviour — a violated precondition
// in an infallible operation should crash — but it means such assertions can
// only be observed from another process. tests/support/death_test.hpp exists
// for exactly those, and the split is not a workaround: it mirrors a real
// distinction between assertions that are recoverable to a caller and ones that
// are not.
using assert_handler_fn = void (*)(const assert_context&);

[[noreturn]] DFR_COLD inline void default_assert_handler(
    const assert_context& ctx) {
  // stderr is unbuffered and needs no allocation. Deliberately not iostreams.
  std::fprintf(stderr, "dfr: assertion failed\n  %s:%u: %s\n  condition: %.*s\n",
               ctx.where.file_name(), ctx.where.line(),
               ctx.where.function_name(),
               static_cast<int>(ctx.expression.size()), ctx.expression.data());
  if (!ctx.message.empty()) {
    std::fprintf(stderr, "  because:   %.*s\n",
                 static_cast<int>(ctx.message.size()), ctx.message.data());
  }
  std::fflush(stderr);
  std::abort();
}

// Replaceable so that the test suite can observe a failure instead of dying,
// and so that an embedding application can route failures into its own crash
// reporter.
//
// This is mutable global state, which the deterministic core otherwise
// forbids. The rule: it may be written only during process start-up, before
// any simulation runs. It is never written from a simulation, so it cannot
// affect a seeded replay.
inline assert_handler_fn g_assert_handler = &default_assert_handler;

// Out of line and cold: the caller keeps a minimal prologue and the hot path
// keeps its registers. quill applies the same treatment to every bail-out in
// its logging front end.
DFR_NOINLINE DFR_COLD inline void report_assert_failure(
    std::string_view expression, std::string_view message,
    const std::source_location& where) {
  const assert_context ctx{expression, message, where};
  g_assert_handler(ctx);

  // Reached only if a handler returned, which its contract forbids. Crash
  // rather than continue with broken invariants.
  default_assert_handler(ctx);
}

}  // namespace detail

// Install a handler. Returns the previous one, so a test can restore it.
//
// Context: process start-up only. Not thread-safe, and not to be called from a
// running simulation.
inline detail::assert_handler_fn set_assert_handler(
    detail::assert_handler_fn handler) {
  detail::assert_handler_fn previous = detail::g_assert_handler;
  detail::g_assert_handler =
      handler != nullptr ? handler : &detail::default_assert_handler;
  return previous;
}

}  // namespace dfr::inline v1

// ---------------------------------------------------------------------------
// The macros
//
// Two forms, because TIGER_STYLE asks for a sentence on the assertions that
// carry weight:
//
//   DFR_ASSERT(header.length >= kHeaderSize);
//   DFR_ASSERT(index < count, "caller must bounds-check before decoding");
//
// Compound conditions are deliberately not supported by a separate macro:
// prefer `DFR_ASSERT(a); DFR_ASSERT(b);` over `DFR_ASSERT(a && b)`, because the
// former reports which half failed.
// ---------------------------------------------------------------------------

#define DFR_DETAIL_ASSERT_IMPL(cond, msg)                                      \
  do {                                                                        \
    if (!(cond)) DFR_UNLIKELY {                                               \
      ::dfr::detail::report_assert_failure(#cond, (msg),                      \
                                          std::source_location::current());   \
    }                                                                         \
  } while (false)

// Consumes the condition so that -Wunused does not fire on variables that
// exist only to be asserted, and so that the expression is still type-checked
// at every level. `sizeof` keeps it unevaluated: an assertion must never have
// side effects, and at level `off` it must cost nothing.
#define DFR_DETAIL_ASSERT_DISABLED(cond, msg)                                  \
  do {                                                                        \
    static_cast<void>(sizeof(decltype(static_cast<bool>(cond))));             \
    static_cast<void>(sizeof(msg));                                           \
  } while (false)

#define DFR_DETAIL_ASSERT_SELECT(_1, _2, NAME, ...) NAME

// clang-format off
#if DFR_ASSERTION_LEVEL >= DFR_ASSERTION_LEVEL_FAST
#  define DFR_DETAIL_ASSERT_1(cond)      DFR_DETAIL_ASSERT_IMPL(cond, "")
#  define DFR_DETAIL_ASSERT_2(cond, msg) DFR_DETAIL_ASSERT_IMPL(cond, msg)
#else
#  define DFR_DETAIL_ASSERT_1(cond)      DFR_DETAIL_ASSERT_DISABLED(cond, "")
#  define DFR_DETAIL_ASSERT_2(cond, msg) DFR_DETAIL_ASSERT_DISABLED(cond, msg)
#endif

#if DFR_ASSERTION_LEVEL >= DFR_ASSERTION_LEVEL_PARANOID
#  define DFR_DETAIL_PARANOID_1(cond)      DFR_DETAIL_ASSERT_IMPL(cond, "")
#  define DFR_DETAIL_PARANOID_2(cond, msg) DFR_DETAIL_ASSERT_IMPL(cond, msg)
#else
#  define DFR_DETAIL_PARANOID_1(cond)      DFR_DETAIL_ASSERT_DISABLED(cond, "")
#  define DFR_DETAIL_PARANOID_2(cond, msg) DFR_DETAIL_ASSERT_DISABLED(cond, msg)
#endif
// clang-format on

// Enabled at `fast` and above. For bounds checks, frame invariants, and
// pre/postconditions whose cost is O(1).
#define DFR_ASSERT(...)                                                        \
  DFR_DETAIL_ASSERT_SELECT(__VA_ARGS__, DFR_DETAIL_ASSERT_2,                   \
                           DFR_DETAIL_ASSERT_1)                                \
  (__VA_ARGS__)

// Enabled at `paranoid` only. For checks whose cost is not O(1): walking a
// whole book to verify an aggregate, recomputing a checksum over a buffer,
// re-deriving state from a log.
//
// FoundationDB gates the equivalent behind a 5% probability so that expensive
// validation runs sometimes in every configuration. We use a build level
// instead, because a seeded replay must execute the same checks as the run it
// is reproducing.
#define DFR_ASSERT_PARANOID(...)                                               \
  DFR_DETAIL_ASSERT_SELECT(__VA_ARGS__, DFR_DETAIL_PARANOID_2,                 \
                           DFR_DETAIL_PARANOID_1)                              \
  (__VA_ARGS__)

// Assert the negative space.
//
// TIGER_STYLE: "The golden rule of assertions is to assert the positive space
// that you do expect AND to assert the negative space that you do not expect,
// because where data moves across the valid/invalid boundary between these
// spaces is where interesting bugs are often found."
//
// DFR_MAYBE states that a condition is *permitted* to be either true or false
// here, and that this was considered rather than overlooked. It generates no
// code. Its value is that it survives review and refactoring in a way a
// comment does not: if the surrounding logic later makes the state impossible,
// the DFR_MAYBE is the thing a reader asks about.
#define DFR_MAYBE(cond) static_cast<void>(sizeof(decltype(static_cast<bool>(cond))))

// Control flow that must not be reachable.
//
// At `off` this is __builtin_unreachable, which lets the optimiser delete the
// path. At `fast` and above it traps first, because a wrong assumption about
// reachability that the optimiser has been told to trust is very hard to debug.
#if DFR_ASSERTION_LEVEL >= DFR_ASSERTION_LEVEL_FAST
#  define DFR_UNREACHABLE(msg)                                                 \
    do {                                                                      \
      ::dfr::detail::report_assert_failure("unreachable", (msg),               \
                                           std::source_location::current());   \
      DFR_UNREACHABLE_INTRINSIC();                                             \
    } while (false)
#else
#  define DFR_UNREACHABLE(msg)                                                 \
    do {                                                                      \
      static_cast<void>(sizeof(msg));                                         \
      DFR_UNREACHABLE_INTRINSIC();                                            \
    } while (false)
#endif

// ---------------------------------------------------------------------------
// Invariant guards
//
// Pairs an object's invariant check on entry and exit of a scope. This is how
// the "assert every property on two different code paths" rule gets applied
// cheaply: a mutating public method wraps itself, and the invariant is checked
// both before and after the mutation without writing it twice.
//
// Costs nothing below `paranoid` — the guard type becomes empty and the
// constructor and destructor are trivially inlined away.
// ---------------------------------------------------------------------------

namespace dfr::inline v1 {

// Requires `obj.check_invariants()` to be callable and side-effect free.
template <typename T>
class invariant_guard {
 public:
  explicit invariant_guard(const T& obj DFR_LIFETIME_BOUND) : obj_(&obj) {
    if constexpr (kParanoidAssertionsEnabled) {
      obj_->check_invariants();
    }
  }

  ~invariant_guard() {
    if constexpr (kParanoidAssertionsEnabled) {
      obj_->check_invariants();
    }
  }

  invariant_guard(const invariant_guard&) = delete;
  invariant_guard& operator=(const invariant_guard&) = delete;
  invariant_guard(invariant_guard&&) = delete;
  invariant_guard& operator=(invariant_guard&&) = delete;

 private:
  const T* obj_;
};

}  // namespace dfr::inline v1

// Two-level indirection: ## suppresses expansion of its operands, so a single
// level would name every guard dfr_invariant_guard___LINE__ and two guards in
// one scope would collide.
#define DFR_DETAIL_CONCAT_(a, b) a##b
#define DFR_DETAIL_CONCAT(a, b) DFR_DETAIL_CONCAT_(a, b)

// Usage: DFR_INVARIANT_GUARD(*this); as the first statement of a mutator.
#define DFR_INVARIANT_GUARD(obj)                                               \
  const ::dfr::invariant_guard DFR_DETAIL_CONCAT(dfr_invariant_guard_,         \
                                                 __LINE__) { (obj) }

#endif  // DFR_CORE_ASSERT_HPP

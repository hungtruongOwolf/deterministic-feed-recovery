#include <dfr/core/assert.hpp>
#include <dfr/core/invariant_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace {

// Thrown by the test handler so that a failed assertion can be observed rather
// than ending the process. A handler is contractually forbidden from
// *returning*; throwing satisfies that, and is the only way to write a positive
// test for the failure path without a death-test facility.
struct assertion_fired {
  std::string expression;
  std::string message;
  std::string file;
  unsigned line{};
};

void throwing_handler(const dfr::assert_context& ctx) {
  throw assertion_fired{std::string{ctx.expression}, std::string{ctx.message},
                        std::string{ctx.where.file_name()}, ctx.where.line()};
}

// Installs the throwing handler for one scope and restores whatever was there.
// Restoring rather than resetting to the default matters: Catch2 may run these
// tests in any order, and a leaked handler would turn an unrelated later
// failure into a confusing exception.
class scoped_throwing_handler {
 public:
  scoped_throwing_handler()
      : previous_(dfr::set_assert_handler(&throwing_handler)) {}
  ~scoped_throwing_handler() { dfr::set_assert_handler(previous_); }

  scoped_throwing_handler(const scoped_throwing_handler&) = delete;
  scoped_throwing_handler& operator=(const scoped_throwing_handler&) = delete;
  // Moving would leave two guards racing to restore the same previous handler on destruction; deleted for the
  // same reason copying is, not left implicitly deleted by omission.
  scoped_throwing_handler(scoped_throwing_handler&&) = delete;
  scoped_throwing_handler& operator=(scoped_throwing_handler&&) = delete;

 private:
  dfr::detail::assert_handler_fn previous_;
};

// Counts calls so the invariant guard can be observed. Deliberately a mutable
// counter behind a const method: check_invariants() must be logically
// side-effect free, and `mutable` is how we observe it without making the
// method non-const and thus unusable by the guard.
struct counted_invariants {
  mutable int checks{0};
  bool healthy{true};

  void check_invariants() const {
    ++checks;
    DFR_ASSERT(healthy, "counted_invariants was left in a broken state");
  }
};

// Returns true and records that it ran, so a test can prove that a disabled
// assertion did not evaluate its condition.
bool note_evaluation(bool& flag) {
  flag = true;
  return true;
}

}  // namespace

TEST_CASE("a satisfied assertion is silent", "[core][assert]") {
  const scoped_throwing_handler guard;

  // The macros expand to `do { ... } while (false)`, a statement rather than an
  // expression, so Catch2's expression-taking macros need a lambda around them.
  // Keeping them statements is deliberate: it forces a semicolon at the call
  // site and prevents an assertion being used as a subexpression.
  CHECK_NOTHROW([] { DFR_ASSERT(1 + 1 == 2); }());
  CHECK_NOTHROW([] { DFR_ASSERT(1 + 1 == 2, "arithmetic still works"); }());
  CHECK_NOTHROW([] { DFR_ASSERT_PARANOID(1 + 1 == 2); }());
}

TEST_CASE("a failed assertion reports its expression and location",
          "[core][assert]") {
  if constexpr (!dfr::kAssertionsEnabled) {
    SUCCEED("assertions are compiled out at this level");
    return;
  }

  const scoped_throwing_handler guard;

  try {
    DFR_ASSERT(2 + 2 == 5);
    FAIL("the assertion should have fired");
  } catch (const assertion_fired& fired) {
    // The expression text is the whole point of the macro; a handler that only
    // received a bool would be useless for diagnosis.
    CHECK(fired.expression == "2 + 2 == 5");
    CHECK(fired.message.empty());
    CHECK(std::string_view{fired.file}.find("assert_test.cpp") !=
          std::string_view::npos);
    CHECK(fired.line > 0);
  }
}

TEST_CASE("a failed assertion carries its message", "[core][assert]") {
  if constexpr (!dfr::kAssertionsEnabled) {
    SUCCEED("assertions are compiled out at this level");
    return;
  }

  const scoped_throwing_handler guard;

  try {
    DFR_ASSERT(false, "sequence numbers must not go backwards");
    FAIL("the assertion should have fired");
  } catch (const assertion_fired& fired) {
    CHECK(fired.expression == "false");
    CHECK(fired.message == "sequence numbers must not go backwards");
  }
}

TEST_CASE("two assertions on the same line report distinct expressions",
          "[core][assert]") {
  if constexpr (!dfr::kAssertionsEnabled) {
    SUCCEED("assertions are compiled out at this level");
    return;
  }

  const scoped_throwing_handler guard;

  // TIGER_STYLE prefers `assert(a); assert(b);` over `assert(a and b)` so that
  // a failure names which half broke. Pin that this actually works.
  try {
    DFR_ASSERT(true);
    DFR_ASSERT(false, "the second one");
    FAIL("the second assertion should have fired");
  } catch (const assertion_fired& fired) {
    CHECK(fired.message == "the second one");
  }
}

TEST_CASE("DFR_UNREACHABLE traps in a checked build", "[core][assert]") {
  if constexpr (!dfr::kAssertionsEnabled) {
    SUCCEED("DFR_UNREACHABLE is __builtin_unreachable at this level, and "
            "reaching it is undefined rather than observable");
    return;
  }

  const scoped_throwing_handler guard;

  const auto reach_it = [] {
    // A switch whose default is genuinely impossible is the shape this exists
    // for; here we force it directly.
    DFR_UNREACHABLE("a fault op outside the enumerated set");
  };

  try {
    reach_it();
    FAIL("DFR_UNREACHABLE should have fired");
  } catch (const assertion_fired& fired) {
    CHECK(fired.expression == "unreachable");
    CHECK(fired.message == "a fault op outside the enumerated set");
  }
}

TEST_CASE("set_assert_handler round-trips and nullptr restores the default",
          "[core][assert]") {
  const dfr::detail::assert_handler_fn original =
      dfr::set_assert_handler(&throwing_handler);
  CHECK(original != nullptr);

  const dfr::detail::assert_handler_fn observed =
      dfr::set_assert_handler(nullptr);
  CHECK(observed == &throwing_handler);

  // Passing nullptr must install the default rather than leaving a null
  // pointer that would be called on the next failure.
  const dfr::detail::assert_handler_fn after_null =
      dfr::set_assert_handler(original);
  CHECK(after_null == &dfr::detail::default_assert_handler);
}

TEST_CASE("assertion levels are ordered and consistent", "[core][assert]") {
  STATIC_REQUIRE(DFR_ASSERTION_LEVEL_OFF < DFR_ASSERTION_LEVEL_FAST);
  STATIC_REQUIRE(DFR_ASSERTION_LEVEL_FAST < DFR_ASSERTION_LEVEL_PARANOID);

  // Paranoid implies fast. A level that enabled the expensive checks but not
  // the cheap ones would be incoherent.
  STATIC_REQUIRE(!dfr::kParanoidAssertionsEnabled || dfr::kAssertionsEnabled);
}

TEST_CASE("DFR_ASSERT_PARANOID fires only at the paranoid level",
          "[core][assert]") {
  const scoped_throwing_handler guard;

  if constexpr (dfr::kParanoidAssertionsEnabled) {
    CHECK_THROWS_AS([] { DFR_ASSERT_PARANOID(false, "expensive check"); }(),
                    assertion_fired);
  } else {
    CHECK_NOTHROW([] { DFR_ASSERT_PARANOID(false, "expensive check"); }());
  }
}

TEST_CASE("a disabled assertion does not evaluate its condition",
          "[core][assert]") {
  const scoped_throwing_handler guard;

  bool evaluated = false;

  // Take the address unconditionally. Without this, the `off` build fails under
  // -Werror with -Wunneeded-internal-declaration, because the only reference to
  // note_evaluation is inside an unevaluated `sizeof`.
  //
  // That diagnostic is worth understanding rather than merely silencing: it is
  // the compiler independently confirming what this test asserts, that a
  // disabled assertion does not evaluate its condition. If this line is ever
  // removed and the `off` build still compiles, the disabling mechanism has
  // started evaluating conditions and this test has stopped being meaningful.
  static_cast<void>(&note_evaluation);

  // At `off` the condition must not run at all: an assertion with a side
  // effect would make the build modes behave differently, which for a
  // deterministic library is worse than a missing check.
  DFR_ASSERT(note_evaluation(evaluated));

  if constexpr (dfr::kAssertionsEnabled) {
    CHECK(evaluated);
  } else {
    CHECK_FALSE(evaluated);
  }
}

TEST_CASE("DFR_MAYBE never evaluates its condition", "[core][assert]") {
  // Must stay non-const to bind to note_evaluation's bool&, even though DFR_MAYBE's sizeof(...) context means
  // that call is never actually made.
  // NOLINTNEXTLINE(misc-const-correctness)
  bool evaluated = false;

  // DFR_MAYBE documents that either outcome is permitted here. It must be free
  // at every level, including paranoid, so it can be sprinkled liberally.
  DFR_MAYBE(note_evaluation(evaluated));

  CHECK_FALSE(evaluated);
}

TEST_CASE("the invariant guard checks on entry and on exit",
          "[core][assert]") {
  const counted_invariants subject;

  {
    DFR_INVARIANT_GUARD(subject);
    if constexpr (dfr::kParanoidAssertionsEnabled) {
      CHECK(subject.checks == 1);
    } else {
      CHECK(subject.checks == 0);
    }
  }

  if constexpr (dfr::kParanoidAssertionsEnabled) {
    CHECK(subject.checks == 2);
  } else {
    CHECK(subject.checks == 0);
  }
}

TEST_CASE("two invariant guards can share a scope", "[core][assert]") {
  // The guard variable is named from __LINE__, so two guards on *different*
  // lines must not collide. This is the bug the two-level macro indirection
  // exists to prevent.
  const counted_invariants first;
  const counted_invariants second;

  {
    DFR_INVARIANT_GUARD(first);
    DFR_INVARIANT_GUARD(second);
  }

  if constexpr (dfr::kParanoidAssertionsEnabled) {
    CHECK(first.checks == 2);
    CHECK(second.checks == 2);
  } else {
    CHECK(first.checks == 0);
    CHECK(second.checks == 0);
  }
}

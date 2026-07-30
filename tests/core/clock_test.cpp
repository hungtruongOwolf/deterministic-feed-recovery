#include <dfr/core/clock.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

using namespace std::chrono_literals;

namespace {

// A component written the way every dfr component will be: templated on the
// clock, holding a reference, never naming a platform clock. The point of the
// test is that this compiles and behaves identically against both clocks.
template <dfr::clock_source Clock>
class timeout_watcher {
 public:
  explicit timeout_watcher(const Clock& clock, dfr::duration budget)
      : clock_(&clock), deadline_(dfr::deadline<Clock>::after(clock, budget)) {}

  [[nodiscard]] bool has_expired() const { return deadline_.expired(*clock_); }
  [[nodiscard]] dfr::duration left() const { return deadline_.remaining(*clock_); }

 private:
  const Clock* clock_;
  dfr::deadline<Clock> deadline_;
};

// Negative fixtures for the concept. At namespace scope because a local class
// may not have static data members, which is_steady is.
struct not_a_clock {
  int now() const { return 0; }
};

struct no_now {
  using duration = dfr::duration;
  using time_point = std::chrono::time_point<no_now, duration>;
  static constexpr bool is_steady = true;
};

}  // namespace

TEST_CASE("a manual clock starts at its epoch and only moves when told",
          "[core][clock]") {
  dfr::manual_clock clock;

  CHECK(clock.now().time_since_epoch() == dfr::duration::zero());

  // Reading it twice must give the same answer. This is the property the whole
  // design exists for: a real clock cannot promise it, and a component that
  // reads "now" twice in one step would otherwise see time move underneath it.
  const auto first = clock.now();
  const auto second = clock.now();
  CHECK(first == second);

  clock.advance(100ns);
  CHECK(clock.now().time_since_epoch() == 100ns);
  CHECK(clock.now() > first);
}

TEST_CASE("advancing accumulates and advance_to jumps", "[core][clock]") {
  dfr::manual_clock clock;

  clock.advance(10ns);
  clock.advance(15ns);
  CHECK(clock.now().time_since_epoch() == 25ns);

  // advance_to is the simulator's primary operation: compute the next due
  // event, move straight there. Wall time is then O(events), not O(simulated
  // duration).
  clock.advance_to(dfr::manual_clock::time_point{1s});
  CHECK(clock.now().time_since_epoch() == 1s);

  // A zero advance is legal — two events at the same timestamp are ordinary.
  clock.advance(0ns);
  CHECK(clock.now().time_since_epoch() == 1s);
  clock.advance_to(clock.now());
  CHECK(clock.now().time_since_epoch() == 1s);
}

TEST_CASE("a clock constructed at an offset reports it", "[core][clock]") {
  const dfr::manual_clock clock{dfr::manual_clock::time_point{5s}};
  CHECK(clock.now().time_since_epoch() == 5s);
}

TEST_CASE("time never runs backwards", "[core][clock]") {
  // The reason this is asserted rather than clamped: a clock that can go
  // backwards makes every deadline comparison in the library unsound, and the
  // failure surfaces far from the call that caused it.
  //
  // A death test rather than a throwing handler, because advance() is noexcept.
  // Throwing out of a noexcept function calls std::terminate, so the exception
  // never reaches a catch site — the process aborts, which is the correct
  // behaviour and is only observable from another process. See
  // tests/support/death_test.hpp.
  DFR_CHECK_ABORTS({
    dfr::manual_clock clock;
    clock.advance(100ns);
    clock.advance(-1ns);
  });

  DFR_CHECK_ABORTS({
    dfr::manual_clock clock;
    clock.advance(100ns);
    clock.advance_to(dfr::manual_clock::time_point{50ns});
  });
}

TEST_CASE("a deadline in the past is rejected at construction",
          "[core][clock]") {
  DFR_CHECK_ABORTS({
    const dfr::manual_clock clock;
    const auto d = dfr::deadline<dfr::manual_clock>::after(clock, -1ns);
    static_cast<void>(d);
  });
}

TEST_CASE("the real clock is steady and moves forward", "[core][clock]") {
  const dfr::real_clock clock;

  const auto first = clock.now();
  const auto second = clock.now();

  // steady_clock, so it must be monotonic. Not asserted to be *strictly*
  // increasing: two calls can land in the same nanosecond bucket.
  CHECK(second >= first);
  STATIC_REQUIRE(dfr::real_clock::is_steady);
}

TEST_CASE("time points from different clocks do not mix", "[core][clock]") {
  // This is what the tag-type approach buys, and the reason it is worth not
  // satisfying std::chrono::is_clock. A component that accidentally compares
  // simulated time against real time is a compile error, not a scheduling bug
  // that shows up under load.
  STATIC_REQUIRE_FALSE(std::is_convertible_v<dfr::manual_clock::time_point,
                                             dfr::real_clock::time_point>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<dfr::real_clock::time_point,
                                             dfr::manual_clock::time_point>);

  // Durations, by contrast, are shared: a five-microsecond budget means the same
  // thing on either timeline.
  STATIC_REQUIRE(std::is_same_v<dfr::manual_clock::duration,
                                dfr::real_clock::duration>);
}

TEST_CASE("both clocks satisfy the concept", "[core][clock]") {
  STATIC_REQUIRE(dfr::clock_source<dfr::manual_clock>);
  STATIC_REQUIRE(dfr::clock_source<dfr::real_clock>);

  // And something that merely has a now() of the wrong shape does not.
  STATIC_REQUIRE_FALSE(dfr::clock_source<not_a_clock>);

  // no_now has every requirement except now(), so this pins that the concept
  // rejects it for the right reason rather than because a type alias is absent.
  STATIC_REQUIRE(no_now::is_steady);
  STATIC_REQUIRE_FALSE(dfr::clock_source<no_now>);
}

// ---------------------------------------------------------------------------
// deadline
// ---------------------------------------------------------------------------

TEST_CASE("a deadline expires at exactly its instant", "[core][clock]") {
  dfr::manual_clock clock;
  const auto d = dfr::deadline<dfr::manual_clock>::after(clock, 100ns);

  CHECK_FALSE(d.expired(clock));

  clock.advance(99ns);
  CHECK_FALSE(d.expired(clock));
  CHECK(d.remaining(clock) == 1ns);

  // The boundary decision, made once here rather than at every call site: "at
  // exactly the deadline" counts as expired.
  clock.advance(1ns);
  CHECK(d.expired(clock));
  CHECK(d.remaining(clock) == 0ns);
}

TEST_CASE("remaining goes negative so lateness keeps its magnitude",
          "[core][clock]") {
  dfr::manual_clock clock;
  const auto d = dfr::deadline<dfr::manual_clock>::after(clock, 10ns);

  clock.advance(35ns);

  // Clamping to zero would lose the information a retransmit-timeout log needs:
  // being 25ns late and being 25ms late are very different diagnoses.
  CHECK(d.expired(clock));
  CHECK(d.remaining(clock) == -25ns);
}

TEST_CASE("never() does not expire", "[core][clock]") {
  dfr::manual_clock clock;
  const auto d = dfr::deadline<dfr::manual_clock>::never();

  CHECK(d.is_never());
  CHECK_FALSE(d.expired(clock));

  // Preferred over optional<deadline> precisely so the hot path has no branch
  // on whether a timeout was configured. Advancing a long way must not wrap
  // into expiry.
  clock.advance(std::chrono::hours{24 * 365});
  CHECK_FALSE(d.expired(clock));
}

TEST_CASE("deadlines from one clock are ordered", "[core][clock]") {
  dfr::manual_clock clock;
  const auto sooner = dfr::deadline<dfr::manual_clock>::after(clock, 10ns);
  const auto later = dfr::deadline<dfr::manual_clock>::after(clock, 20ns);

  CHECK(sooner < later);
  CHECK(sooner != later);
  CHECK(sooner == dfr::deadline<dfr::manual_clock>{sooner.at()});
  CHECK(later < dfr::deadline<dfr::manual_clock>::never());
}

TEST_CASE("a component written against the concept works on both clocks",
          "[core][clock]") {
  // The acceptance test for the whole header: production and simulation differ
  // by a template argument, and nothing in the component changes.
  {
    dfr::manual_clock clock;
    const timeout_watcher watcher{clock, 500ns};

    CHECK_FALSE(watcher.has_expired());
    clock.advance(499ns);
    CHECK_FALSE(watcher.has_expired());
    clock.advance(1ns);
    CHECK(watcher.has_expired());
  }
  {
    const dfr::real_clock clock;
    const timeout_watcher watcher{clock, std::chrono::hours{1}};

    // Not expired, and no sleeping in a unit test to find out.
    CHECK_FALSE(watcher.has_expired());
    CHECK(watcher.left() > 0ns);
  }
}

TEST_CASE("the manual clock is usable at compile time", "[core][clock]") {
  // A constexpr clock means a decoder's timing logic can be exercised in a
  // static_assert, with no fixture at all.
  constexpr auto advanced = [] {
    dfr::manual_clock clock;
    clock.advance(7ns);
    clock.advance_to(dfr::manual_clock::time_point{42ns});
    return clock.now().time_since_epoch();
  }();

  STATIC_REQUIRE(advanced == dfr::duration{42});
}

// Clocks.
//
// Time is injected, never read from the platform. Every component that needs
// "now" is templated on a clock and holds a reference to one, so that the
// simulator can drive time forwards in jumps and a seeded run replays exactly.
// BUILD-GUIDE.md section 11.6 lists reading a real clock as determinism leak
// number one, and it is the leak that is hardest to find later because the
// symptom is a test that passes ninety-nine times.
//
// The zero-cost part is copied from seastar: its manual_clock supplies the
// std::chrono type aliases and `timer<Clock>` calls Clock::now() statically, so
// simulated time costs nothing at all — no virtual call, unlike quill's
// UserClockSource::now() or FoundationDB's INetwork::now(), which both pay one
// per query.
//
// The part that is deliberately *not* seastar's:
//
//   seastar's manual_clock has a `static now()` and therefore static mutable
//   state, because that is what std::chrono's Clock requirements demand. This
//   library forbids mutable global state in the deterministic core, and a
//   global clock is the worst possible instance of it: two simulations in one
//   process would silently share a timeline.
//
//   std::chrono::time_point<Clock, Duration> only uses Clock as a *tag*; it
//   never calls Clock::now(). So a clock can be an ordinary object with a
//   non-static now(), and still produce time_points that will not implicitly
//   convert between clocks. That gives the type safety without the global.
//
// The cost of that choice is that these clocks do not satisfy
// std::chrono::is_clock, so they cannot be handed to std::chrono utilities that
// require it. Nothing here needs to be.

#ifndef DFR_CORE_CLOCK_HPP
#define DFR_CORE_CLOCK_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <ratio>

namespace dfr::inline v1 {

// Nanoseconds, signed, 64-bit. Signed because a difference of two time points
// is naturally signed and because an unsigned duration turns "earlier than" into
// a huge positive number. 64 bits of nanoseconds is 292 years, which outlasts
// any session.
using duration = std::chrono::duration<std::int64_t, std::nano>;

static_assert(duration::max().count() > 0,
              "duration must be signed with a positive max");

// What a component may assume about the clock it was given.
//
// Note the absence of a `sleep`. A component that can sleep can hide a
// dependency on real time; the simulator's answer to "wait" is to schedule and
// return, per TIGER_STYLE's rule that a program should run at its own pace
// rather than react directly to external events.
template <typename C>
concept clock_source = requires(const C& clock) {
  typename C::duration;
  typename C::time_point;
  { clock.now() } -> std::same_as<typename C::time_point>;
  { C::is_steady } -> std::convertible_to<bool>;
};

// ---------------------------------------------------------------------------
// manual_clock — time as data
// ---------------------------------------------------------------------------

// A clock that only moves when told to. The simulator advances it to the
// timestamp of the next scheduled event, which is why a simulated run is
// independent of how long the work actually takes: wall time is O(events), not
// O(simulated duration).
class manual_clock {
 public:
  using duration = dfr::duration;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<manual_clock, duration>;

  static constexpr bool is_steady = true;

  constexpr manual_clock() noexcept = default;
  explicit constexpr manual_clock(time_point start) noexcept : now_(start) {}

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr time_point now() const noexcept {
    return now_;
  }

  // Move forward by a non-negative amount.
  //
  // Rejecting a negative delta rather than accepting it is the whole value of
  // this type: a clock that can go backwards makes every "has the deadline
  // passed" comparison in the library unsound, and the bug surfaces far from
  // the call that caused it.
  constexpr void advance(duration delta) noexcept {
    DFR_ASSERT(delta >= duration::zero(),
               "a clock must not move backwards; the simulator advances to the "
               "next event, it never rewinds");
    DFR_ASSERT(now_.time_since_epoch() <=
                   time_point::max().time_since_epoch() - delta,
               "advancing would overflow the time point");
    now_ += delta;
  }

  // Jump to an absolute point. The simulator's primary operation: it computes
  // the next due event and moves straight there.
  constexpr void advance_to(time_point when) noexcept {
    DFR_ASSERT(when >= now_,
               "advance_to must not rewind; check the timer queue ordering");
    now_ = when;
  }

  // Only for constructing a fresh timeline, and deliberately named so that it
  // reads as suspicious at a call site inside a simulation.
  constexpr void reset_to(time_point when) noexcept { now_ = when; }

  [[nodiscard]] friend constexpr bool operator==(const manual_clock&,
                                                 const manual_clock&) = default;

 private:
  time_point now_{};
};

static_assert(clock_source<manual_clock>);

// ---------------------------------------------------------------------------
// real_clock — the production clock
// ---------------------------------------------------------------------------

// std::chrono::steady_clock, wrapped so that it presents the same interface as
// manual_clock and so that no dfr header ever names a platform clock directly.
//
// steady_clock rather than system_clock: system_clock can jump when the
// operating system's notion of the wall time is corrected, and a feed handler
// that measured a gap timeout across an NTP step would abandon a recoverable
// gap.
class real_clock {
 public:
  using duration = dfr::duration;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<real_clock, duration>;

  static constexpr bool is_steady = true;

  [[nodiscard]] time_point now() const noexcept {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return time_point{std::chrono::duration_cast<duration>(since_epoch)};
  }
};

static_assert(clock_source<real_clock>);

// A time point from one clock must not silently become one from another. This is
// what makes the tag-type approach worth its cost: a component accidentally
// mixing simulated and real time is a compile error rather than a subtle
// scheduling bug.
static_assert(!std::is_convertible_v<manual_clock::time_point,
                                     real_clock::time_point>);
static_assert(!std::is_convertible_v<real_clock::time_point,
                                     manual_clock::time_point>);

// ---------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------

// A point in time by which something must happen, plus the question every
// caller actually asks. Templated on the clock so a deadline from one timeline
// cannot be compared against another's.
//
// Exists as a named type rather than a bare time_point because a raw comparison
// gets the boundary wrong roughly half the time: whether "at exactly the
// deadline" counts as expired is a decision that should be made once, here, and
// not re-litigated at every call site. It counts as expired.
template <clock_source Clock>
class deadline {
 public:
  using time_point = typename Clock::time_point;

  constexpr deadline() noexcept = default;
  explicit constexpr deadline(time_point when) noexcept : when_(when) {}

  // The common spelling: "N nanoseconds from now".
  [[nodiscard]] static constexpr deadline after(const Clock& clock,
                                                dfr::duration delta) noexcept {
    DFR_ASSERT(delta >= dfr::duration::zero(),
               "a deadline in the past is a programmer error; use expired()");
    return deadline{clock.now() + delta};
  }

  // A deadline that never expires, for a component configured without a timeout.
  // Preferred over an optional<deadline> so that the hot path has no branch on
  // whether a timeout is configured.
  [[nodiscard]] static constexpr deadline never() noexcept {
    return deadline{time_point::max()};
  }

  [[nodiscard]] constexpr time_point at() const noexcept { return when_; }

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool expired(
      const Clock& clock) const noexcept {
    return clock.now() >= when_;
  }

  // Negative once passed, so a caller can log how late it was rather than
  // clamping to zero and losing the magnitude.
  [[nodiscard]] constexpr dfr::duration remaining(
      const Clock& clock) const noexcept {
    return when_ - clock.now();
  }

  [[nodiscard]] constexpr bool is_never() const noexcept {
    return when_ == time_point::max();
  }

  [[nodiscard]] friend constexpr bool operator==(deadline, deadline) = default;
  [[nodiscard]] friend constexpr auto operator<=>(deadline, deadline) = default;

 private:
  time_point when_{};
};

}  // namespace dfr::inline v1

#endif  // DFR_CORE_CLOCK_HPP

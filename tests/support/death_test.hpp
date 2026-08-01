// Death tests.
//
// Most assertions can be tested in-process by installing a handler that throws
// (see tests/core/assert_test.cpp). That trick has a limit, and the limit is not
// obvious until it bites:
//
//   An assertion inside a `noexcept` function cannot be tested that way. The
//   handler's contract is "must not return", and throwing satisfies it, but
//   throwing out of a noexcept function calls std::terminate, so the whole test
//   binary aborts instead of the exception being caught.
//
// That is the *correct* runtime behaviour: a violated precondition in an
// infallible operation should crash. It just means observing it requires a
// separate process.
//
// Catch2 has no death-test facility, so this is a small POSIX one. It forks,
// runs the callable in the child, and reports how the child ended.

#ifndef DFR_TESTS_SUPPORT_DEATH_TEST_HPP
#define DFR_TESTS_SUPPORT_DEATH_TEST_HPP

#include <csignal>
#include <cstdlib>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#  define DFR_HAS_DEATH_TESTS 1
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  define DFR_HAS_DEATH_TESTS 0
#endif

namespace dfr_test {

enum class death_outcome {
  aborted,        // the child raised SIGABRT, which is what a failed assertion does
  signalled,      // the child died on some other signal
  exited_cleanly, // the child returned normally: the assertion did NOT fire
  unsupported,    // no fork() on this platform
};

#if DFR_HAS_DEATH_TESTS

// Runs `body` in a forked child and reports how the child terminated.
//
// The child writes nothing to stdout: a failed assertion's diagnostic would
// otherwise interleave with the parent's test output and look like a real
// failure. stderr is redirected to /dev/null for the same reason.
template <typename Body>
[[nodiscard]] death_outcome run_in_child(Body&& body) {
  std::fflush(nullptr);  // do not duplicate the parent's buffered output

  const pid_t pid = ::fork();
  if (pid < 0) {
    return death_outcome::unsupported;
  }

  if (pid == 0) {
    // Child. Silence the expected diagnostic, run the body, and if it returns
    // at all then the assertion did not fire.
    if (FILE* null = std::freopen("/dev/null", "w", stderr); null == nullptr) {
      std::_Exit(2);
    }
    std::forward<Body>(body)();
    // _Exit rather than exit: no atexit handlers, no static destructors, so a
    // child that survived cannot corrupt anything the parent shares.
    std::_Exit(0);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    // Retry on EINTR rather than reporting a spurious result.
  }

  if (WIFSIGNALED(status)) {
    return WTERMSIG(status) == SIGABRT ? death_outcome::aborted
                                       : death_outcome::signalled;
  }
  return death_outcome::exited_cleanly;
}

#else

template <typename Body>
[[nodiscard]] death_outcome run_in_child(Body&&) {
  return death_outcome::unsupported;
}

#endif

}  // namespace dfr_test

// Asserts that `stmt` trips a dfr assertion, i.e. that it aborts.
//
// Skips rather than fails where assertions are compiled out or fork is
// unavailable, so the same test file is meaningful in every build configuration.
#define DFR_CHECK_ABORTS(stmt)                                                 \
  do {                                                                        \
    if constexpr (!::dfr::kAssertionsEnabled) {                               \
      SUCCEED("assertions are compiled out at this level");                   \
    } else {                                                                  \
      const auto outcome = ::dfr_test::run_in_child([&] { stmt; });           \
      if (outcome == ::dfr_test::death_outcome::unsupported) {                \
        SUCCEED("death tests are unavailable on this platform");              \
      } else {                                                                \
        CHECK(outcome == ::dfr_test::death_outcome::aborted);                 \
      }                                                                       \
    }                                                                         \
  } while (false)

#endif  // DFR_TESTS_SUPPORT_DEATH_TEST_HPP

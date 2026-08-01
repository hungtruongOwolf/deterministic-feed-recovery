// When to ask for a retransmit, how often, and when to stop asking.
//
// Separate from the requester because these are the numbers an operator tunes per
// venue, and because they are worth reading without the state machine around them.
// Every one of them is a duration or a count: nothing here is a strategy object, so
// a configuration is a value that can be printed, diffed and committed alongside the
// seed that reproduced a failure.
//
// No floating point anywhere, including in the backoff
// ----------------------------------------------------
// The backoff is an integer ratio raised to an integer power, computed with
// saturating multiplication. Doubling a timeout is `numerator = 2, denominator = 1`.
// A float would be more natural to write and would put a rounding decision inside the
// replay path: the build already compiles with -ffp-contract=off because the same
// expression may otherwise be evaluated at two different precisions, and a backoff
// that landed one nanosecond either side of a comparison would make a failing seed
// stop reproducing. Integers cost nothing here and remove the question.

#ifndef DFR_RECOVERY_RETRANSMIT_POLICY_HPP
#define DFR_RECOVERY_RETRANSMIT_POLICY_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/result.hpp>

#include <cstdint>

namespace dfr::inline v1::recovery {

// MoldUDP64's Message Count is 16 bits, and the specification caps a request at
// 60,000 messages rather than 65,535. Requesting more is not "ambitious": the
// retransmit server answers nothing at all, so a client asking for a million messages
// gets silence and then reports a timeout it caused itself. Already enforced by
// wire::moldudp64::clamp_request_count; repeated here as the default so a policy is
// legal on the wire before it reaches an encoder.
inline constexpr std::uint64_t kDefaultMaxMessagesPerRequest = 60'000;

struct retransmit_policy {
  // How long to wait after discovering a hole before asking for anything.
  //
  // Not zero, and this is the substance of NAK suppression rather than a politeness.
  // On a multicast feed most gaps are transient reordering: a datagram arriving a
  // few hundred microseconds late, and a receiver that requests immediately
  // generates recovery traffic for data that was already on its way. Every receiver
  // on the group does it at the same instant, for the same packet, which is how a
  // single dropped datagram becomes a load spike on the retransmit server.
  duration settle_delay{std::chrono::microseconds{500}};

  // How long to wait for a reply before asking again.
  duration first_timeout{std::chrono::milliseconds{50}};

  // The ceiling the backoff grows to, so a long outage does not push the next attempt
  // beyond the retention window and turn a recoverable gap into a snapshot.
  duration max_timeout{std::chrono::milliseconds{1'000}};

  // Integer backoff ratio applied per attempt. 2/1 doubles; 3/2 is gentler; 1/1
  // disables backoff and retries at a fixed interval.
  std::uint32_t backoff_numerator{2};
  std::uint32_t backoff_denominator{1};

  // How many times to ask before giving up on retransmission for this range.
  std::uint32_t max_attempts{5};

  // How long the publisher keeps messages available. Past this, retransmission cannot
  // succeed no matter how many attempts remain, and the only repair is a snapshot.
  // Measured from when the hole was discovered, not from the last attempt: the
  // publisher's window is about the age of the data, not about our persistence.
  duration retention_window{std::chrono::seconds{30}};

  std::uint64_t max_messages_per_request{kDefaultMaxMessagesPerRequest};

  // Checked once, at configuration time, so the state machine can assert rather than
  // branch. Each of these would otherwise fail silently and look like a different
  // bug: a zero denominator divides by zero, zero attempts means a hole is abandoned
  // before it is ever requested, and a retention window shorter than the first
  // timeout means the first attempt is already too late.
  [[nodiscard]] constexpr result<void> validate() const noexcept {
    if (backoff_denominator == 0 || backoff_numerator == 0) {
      return error::invalid_argument;
    }
    if (backoff_numerator < backoff_denominator) {
      // Shrinking timeouts would make a struggling receiver ask faster the longer it
      // failed, which is the opposite of backoff and a way to build a storm.
      return error::invalid_argument;
    }
    if (max_attempts == 0 || max_messages_per_request == 0) {
      return error::invalid_argument;
    }
    if (first_timeout <= duration::zero() || max_timeout < first_timeout) {
      return error::invalid_argument;
    }
    if (settle_delay < duration::zero()) {
      return error::invalid_argument;
    }
    if (retention_window <= settle_delay + first_timeout) {
      return error::invalid_argument;
    }
    return ok();
  }

  // The wait before attempt number `attempt`, counting from 1.
  //
  // Saturating rather than wrapping: the ratio is applied one step at a time and the
  // loop stops the moment it reaches the ceiling, so a large attempt number cannot
  // overflow into a small timeout. A backoff that wrapped would produce the storm the
  // backoff exists to prevent, at exactly the moment things were already going wrong.
  [[nodiscard]] constexpr duration timeout_for(
      std::uint32_t attempt) const noexcept {
    DFR_ASSERT(attempt >= 1, "attempts are counted from one");
    DFR_ASSERT(backoff_denominator != 0, "policy was not validated");

    duration wait = first_timeout;
    for (std::uint32_t step = 1; step < attempt; ++step) {
      if (wait >= max_timeout) {
        return max_timeout;
      }
      const std::int64_t scaled =
          wait.count() / backoff_denominator * backoff_numerator;
      wait = duration{scaled};
      if (wait >= max_timeout) {
        return max_timeout;
      }
    }
    return wait;
  }
};

}  // namespace dfr::inline v1::recovery
#endif  // DFR_RECOVERY_RETRANSMIT_POLICY_HPP

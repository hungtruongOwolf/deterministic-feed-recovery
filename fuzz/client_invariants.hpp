// What must be true of the recovery client after every single call, whatever the program did.
//
// These are the checks, kept apart from the program that drives them so they can be read as a list. Each one is a
// property no sequence of legal calls may break, and each is written so a violation aborts with the sanitiser
// still attached.
//
// The bar for including a property here: it has to be checkable without reimplementing the protocol. A fuzzer
// oracle that models the state machine is a second implementation, and when the two disagree the fuzzer reports
// the model's bug. So these are structural, and the semantics stay in the unit tests and the book oracle where
// there is a real reference to compare against.

#ifndef DFR_FUZZ_CLIENT_INVARIANTS_HPP
#define DFR_FUZZ_CLIENT_INVARIANTS_HPP

#include "program.hpp"

#include <dfr/recovery/client.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace dfr_fuzz {

namespace rec = dfr::recovery;

using fuzz_client = rec::client<dfr::manual_clock, rec::replay_buffer<512, 16>>;

// Aborts, and says which property broke first.
//
// A fuzzer that aborts silently hands back a stack trace through a template instantiation and a lambda, and the
// first thing anybody does with it is add exactly this printf. The name of the property is the useful half of the
// report: the input reproduces it, and the sentence says what to go and read.
inline void require(bool condition, const char* property) noexcept {
  if (!condition) {
    std::fprintf(stderr, "fuzz: broken invariant: %s\n", property);
    std::abort();
  }
}

// Carried between steps, because three of the properties are about change rather than about state.
struct history {
  std::uint64_t delivered_before{0};
  std::uint32_t session{0};
  bool started{false};
  bool has_failed{false};
};

/**
 * Eight properties, checked after every operation.
 *
 * 1. The delivery watermark never goes backwards inside a session. A client that retreats has told a consumer to
 *    un-apply something, and an aggregated book cannot do that. Across a session change it must reset, because
 *    the new session renumbers from its own beginning.
 * 2. It never passes the highest sequence the venue ever published. The watermark is exclusive, so it may equal
 *    that sequence plus one and no more. This is the one property the client cannot
 *    check for itself, and the reason the model exists at all: delivering a number nobody sent is the failure a
 *    recovery client is uniquely able to invent, by mis-computing a range.
 * 3. Failure is absorbing. Once failed, always failed, in every later state read.
 * 4. A failed client keeps saying restart. A caller that polls once and sees idle would assume it is fine.
 * 5. The outstanding holes are canonical: sorted, non-empty, non-overlapping and not adjacent. A gap set that
 *    lost that property would ask for the same range twice, or ask for one that is already filled.
 * 6. `total_missing()` equals the sum of the holes. Two ways of counting the same thing, which is worth
 *    checking precisely because one of them is a cached number.
 * 7. Every hole lies below the tracker's own expectation. A hole is only ever discovered by a later message
 *    arriving, so one above the highest sequence expected would mean the tracker invented it.
 *
 *    Three wrong versions before this one, all mine. First "no hole at or below the delivery watermark", which is
 *    the opposite of the truth: the client keeps delivering behind an open hole on purpose, because stalling on a
 *    gap turns one loss into an outage, so holes below the watermark are the ordinary state of a working client.
 *    Then "every hole below the delivery watermark", which forbids the case where a snapshot is replaying and
 *    arriving packets are held rather than delivered. Seen and delivered are different numbers, and only one of
 *    them bounds a hole.
 * 8. Nothing in the reorder buffer is a sequence the venue never published.
 *
 *    Stated against the venue rather than against the watermark, after two wrong versions. The buffer does not
 *    only hold repairs: while a snapshot is replaying it also holds *live* packets that keep arriving, and those
 *    are above the watermark by construction. Anything tying the buffer to the watermark forbids the ordinary
 *    case, which is how the first two attempts failed.
 */
inline void check(const fuzz_client& client, const venue_model& venue, history& before,
                  bool accepted) noexcept {
  const auto now_delivered = client.delivered_before();
  // The tracker's own expectation, which is what bounds a hole. Not the arbiter's watermark: the two are kept in
  // step deliberately and the library asserts the consequence itself, but they are allowed to differ for the span
  // of a call, and a fuzzer that policed the gap between them would be reporting the ordering of two lines.
  const auto expected = client.tracking().expected_sequence(rec::channel_id::at(0));

  // Monotonic *within a session*. A session change renumbers the feed from its own beginning, so the watermark
  // resets to zero on purpose: carrying it across would classify the new session's first packets as duplicates
  // and the client would sit silent forever. Comparing across that boundary is comparing two different streams.
  const bool same_session = before.started && before.session == venue.session;
  require(!same_session || now_delivered >= before.delivered_before,
          "the delivery watermark went backwards inside one session");
  require(now_delivered <= venue.highest_published + 1,
          "delivered past what the venue ever published");

  const bool failed = client.state() == rec::client_state::failed;
  require(!before.has_failed || failed, "a failed client recovered by itself");

  // One channel: the arbiter merges the lines into a single stream before the tracker sees anything.
  const auto& held = client.tracking().outstanding(rec::channel_id::at(0));

  // Canonical form, stated from outside rather than by calling the private predicate: sorted, non-empty,
  // non-overlapping and non-adjacent. gap_set asserts this internally under paranoid assertions, and a fuzzer
  // that only trusted that assertion would be checking a build configuration rather than the code.
  std::uint64_t summed = 0;
  std::uint64_t previous_end = 0;
  for (const auto hole : held.ranges()) {
    require(!hole.empty(), "an empty hole is outstanding");
    require(previous_end == 0 || hole.first > previous_end,
            "the outstanding holes are not sorted, disjoint and non-adjacent");
    previous_end = hole.end;
    summed += hole.count();
    require(hole.end <= expected, "a hole is outstanding above the tracker's own expectation");
  }
  require(summed == held.total_missing(), "total_missing() disagrees with the holes it counts");

  const auto buffered = client.held().buffered();
  require(buffered.empty() || buffered.end <= venue.highest_published + 1,
          "the reorder buffer holds a message the venue never published");

  before.delivered_before = now_delivered;
  if (accepted) {
    before.session = venue.session;
    before.started = true;
  }
  before.has_failed = failed;
}

/**
 * The two properties about what the client asks for, checked on the decision rather than on the state.
 *
 * A retransmit request must name a range the client is actually missing. Asking for something already delivered
 * wastes a retransmit server's retention window, which is the scarce resource in a real recovery, and asking for
 * something never lost means the range arithmetic is wrong somewhere upstream of the request.
 */
inline void check_decision(const fuzz_client& client, rec::client_decision decision) noexcept {
  if (decision.what == rec::client_action::restart) {
    require(client.state() == rec::client_state::failed, "restart was asked for by a client that has not failed");
    return;
  }
  if (decision.what != rec::client_action::send_retransmit_request) {
    return;
  }
  require(!decision.range.empty(), "an empty range was requested");
  // Every message asked for has to be one the tracker still considers missing. `intersect` returns the part of
  // the outstanding holes that the range covers, so equality of the counts is the statement that the request
  // asks for nothing else.
  const auto missing_in_range =
      client.tracking().outstanding(rec::channel_id::at(0)).intersect(decision.range);
  require(missing_in_range.total_missing() == decision.range.count(),
          "a retransmit was requested for messages that are not missing");
}

}  // namespace dfr_fuzz

#endif  // DFR_FUZZ_CLIENT_INVARIANTS_HPP

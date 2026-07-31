// The book, built on another core, and the same book.
//
// The concurrency analogue of the book oracle, and the assertion that makes the thread boundary load-bearing rather
// than decorative. `spsc_ring` was benchmarked and tested under ThreadSanitizer while **nothing in the architecture
// used it** — which is a worse state than not having one, because it looks like a claim.
//
// What is under test is the whole shape a real feed handler has: recovery on the thread that owns the protocol
// state, a consumer on another core building the book from what crosses. The invariant:
//
//   **the book built across a thread boundary equals the book built single-threaded.**
//
// Nothing on the consumer thread may touch Catch2
// -----------------------------------------------
// This test aborted intermittently — inside `Catch::OutputRedirect::activate`, with no failing expression and a
// SIGABRT that read as a library defect. It was not one: `detail::apply` used REQUIRE, this calls it from the
// consumer thread, and Catch2's result capture is single-threaded. It reproduced on the second run once I looked
// for it, so "the suite passed" had meant "the suite passed once". The consumer now counts and the main thread
// asserts, and `--repeat until-fail` in CI is what turns a rare abort into a build failure.
//
// That is stronger than the ring's own property test. The property test feeds the ring synthetic records in order
// and checks none are lost or reordered. This feeds it a *damaged* feed's deliveries — out of order, with repairs
// arriving after later messages — under real contention, and checks the far side arrives at the right book. A ring
// that lost a record under load, or a publisher that let an older update overtake a newer one, fails here and
// passes there.

#include "support/oracle_replay.hpp"

#include <dfr/concurrent/publisher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>

using namespace dfr_test::oracle;  // NOLINT(google-build-using-namespace)

namespace {

namespace conc = dfr::concurrent;

using feed_publisher = conc::publisher<4096, 256>;

struct threaded_result {
  // Per symbol, because `detail::apply` routes by symbol and the feed uses one — keeping the map means the two
  // files share one apply function rather than having a second, simpler one that could disagree with it.
  std::map<std::string, oracle_book> state_by_symbol;
  oracle_book state;
  std::uint64_t consumed{0};
  /** Copied out of the consumer's own counters before it exits — see replay_result::malformed. */
  std::uint64_t malformed{0};
  conc::publisher_stats produced{};
};

// Runs the recovery replay, but hands every delivered message to a consumer thread instead of applying it here.
//
// The consumer is a loop with no memory: pop, decode, apply. That it *can* be is the point — the publisher put the
// messages in order before they crossed, so the far side needs no buffer and no knowledge of sequence numbers.
threaded_result across_a_boundary(const feed& source, std::uint64_t seed, std::uint32_t faults) {
  threaded_result out;
  feed_publisher producer{1};
  std::atomic<bool> producing{true};

  std::thread consumer([&] {
    std::array<conc::delivery, 64> batch{};
    replay_result sink;  // the consumer's own counters, so the two threads share no mutable state but the ring
    while (producing.load(std::memory_order_acquire) || producer.ring().size_approx() > 0) {
      const auto got = producer.ring().pop_batch(batch.data(), batch.size());
      for (std::size_t i = 0; i < got; ++i) {
        detail::apply(out.state_by_symbol, batch[i].payload(), sink);
        ++out.consumed;
      }
    }
    out.state = out.state_by_symbol[std::string{kTracedSymbolForTest}];
    // Carried across the join rather than asserted here: a Catch2 macro on this thread races the main thread's
    // result capture and aborts inside its output redirect, which is what made this test flaky.
    out.malformed = sink.malformed;
  });

  replay_recovered_into(source, seed, faults, [&](std::uint64_t sequence, dfr::packet_view body) {
    // Refusals are a real condition, not an assertion failure: a full ring means the consumer is behind. The test
    // asserts on the count afterwards, because a run that refused anything cannot claim the books match.
    (void)producer.offer(sequence, 0, false, body);
  });

  producing.store(false, std::memory_order_release);
  consumer.join();
  out.produced = producer.stats();
  return out;
}

}  // namespace

TEST_CASE("the book built across a thread boundary is the same book", "[integration][concurrent]") {
  const auto source = publish_feed(600, 1);
  const auto reference = replay_clean(source);

  for (const std::uint64_t seed : {1ULL, 4711ULL, 90210ULL}) {
    const auto through = across_a_boundary(source, seed, /*faults=*/8);

    // Nothing may have been dropped for want of ring space or record size, or the comparison below is vacuous.
    CHECK(through.produced.refused == 0);
    CHECK(through.produced.oversized == 0);
    // And the reordering must actually have happened, or the run proves nothing about the hard case.
    CHECK(through.produced.reordered > 0);

    CHECK(through.malformed == 0);
    CHECK(through.consumed == through.produced.published);
    CHECK(through.state == reference.books.at(std::string{kTracedSymbolForTest}));
  }
}

TEST_CASE("the publisher orders before the boundary, so the consumer needs no memory",
          "[integration][concurrent]") {
  feed_publisher producer{1};
  const std::array<std::byte, 4> body{};
  const dfr::packet_view view{body.data(), body.size()};

  // Offered 3, 2, 1 — the shape a gap-filling client produces. Nothing may cross until 1 arrives, and then all
  // three must cross in order.
  CHECK(producer.offer(3, 0, false, view));
  CHECK(producer.offer(2, 0, false, view));
  CHECK(producer.ring().size_approx() == 0);
  CHECK(producer.stats().published == 0);

  CHECK(producer.offer(1, 0, false, view));
  CHECK(producer.stats().published == 3);

  std::array<conc::delivery, 8> out{};
  REQUIRE(producer.ring().pop_batch(out.data(), out.size()) == 3);
  CHECK(out[0].sequence == 1);
  CHECK(out[1].sequence == 2);
  CHECK(out[2].sequence == 3);
}

TEST_CASE("a sequence already published is dropped rather than applied twice",
          "[integration][concurrent]") {
  feed_publisher producer{1};
  const std::array<std::byte, 4> body{};
  const dfr::packet_view view{body.data(), body.size()};

  REQUIRE(producer.offer(1, 0, false, view));
  REQUIRE(producer.offer(1, 0, false, view));  // a duplicate the client let through

  // Publishing it again would apply an older update over a newer one, which in an aggregated book is permanent.
  CHECK(producer.stats().published == 1);
}

TEST_CASE("a gap wider than the window is refused, not buffered without limit",
          "[integration][concurrent]") {
  conc::publisher<64, 8> narrow{1};
  const std::array<std::byte, 4> body{};
  const dfr::packet_view view{body.data(), body.size()};

  // The window is eight, so sequences 2..8 can be held while 1 is missing and 9 cannot: a delivery exactly `Pending`
  // ahead of `next_` would land on the slot `next_` is waiting for and overwrite it.
  //
  // That boundary was `>` rather than `>=` when this was written, and CI caught it on x86-64 while it passed here —
  // arm64's timing never produced a reorder distance of exactly the window. So the exact boundary is asserted, not
  // just a distance comfortably past it.
  CHECK(narrow.offer(8, 0, false, view));
  CHECK_FALSE(narrow.offer(9, 0, false, view));
  CHECK_FALSE(narrow.offer(100, 0, false, view));
  CHECK(narrow.stats().refused == 2);
}

TEST_CASE("a ring smaller than the reorder window stalls without corrupting anything",
          "[integration][concurrent]") {
  // A legitimate configuration and one the design did not think through: the drain fills the ring, a publish fails,
  // and entries stay held with `next_` stalled. Everything after that has to be refused and counted rather than
  // written over a slot the window still needs.
  //
  // This is where CI's abort came from — twice, on x86-64, while every interleaving constructed by hand here passed.
  // The fix was to stop arguing about which interleavings can alias a slot and make the check unconditional.
  conc::publisher<4, 8> tight{1};
  const std::array<std::byte, 4> body{};
  const dfr::packet_view view{body.data(), body.size()};

  for (std::uint64_t s = 2; s <= 8; ++s) {
    CHECK(tight.offer(s, 0, false, view));
  }
  // Releasing 1 drains as far as the ring allows and then fails.
  CHECK_FALSE(tight.offer(1, 0, false, view));
  CHECK(tight.stats().published == 4);
  CHECK(tight.stats().refused >= 1);

  // Everything after this either fits the window or is refused. Nothing may abort, and nothing may be published
  // under a sequence it does not have.
  for (std::uint64_t s = 9; s <= 400; ++s) {
    (void)tight.offer(s, 0, false, view);
  }
  CHECK(tight.stats().published == 4);
  CHECK(tight.next_sequence() == 5);

  // And what did cross is still in order and correctly numbered.
  std::array<conc::delivery, 8> out{};
  REQUIRE(tight.ring().pop_batch(out.data(), out.size()) == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(out[i].sequence == i + 1);
  }
}

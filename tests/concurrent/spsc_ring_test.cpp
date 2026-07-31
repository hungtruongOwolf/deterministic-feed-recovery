// The ring, and the two threads it exists for.
//
// Two kinds of test here and they check different things:
//
//   * Single-threaded tests pin the *semantics* — what full means, what empty means, that a refusal writes
//     nothing, that a wrap is invisible. These are deterministic and they are what a reader should read first.
//   * A two-thread test pins the *ordering*. It cannot prove the memory ordering is right — no test can, on
//     one machine, in finite time — but it can fail, and under ThreadSanitizer it fails loudly for the right
//     reason. That is why the tsan preset exists and why it finally has something to run.
//
// The two-thread test checks a property rather than a schedule: every record the consumer sees carries the
// sequence the producer put in it, and they arrive in order, and none is missing or repeated. A test that
// asserted on *timing* would be a test that fails on a busy machine and teaches nobody anything.

#include <dfr/concurrent/delivery.hpp>
#include <dfr/concurrent/spsc_ring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace conc = dfr::concurrent;

namespace {

using small_ring = conc::spsc_ring<std::uint64_t, 4>;
using delivery_ring = conc::spsc_ring<conc::delivery, 1024>;

}  // namespace

TEST_CASE("an empty ring pops nothing and says so") {
  small_ring ring;
  std::uint64_t value = 99;
  CHECK_FALSE(ring.pop(value));
  CHECK(value == 99);  // untouched, not zeroed
  CHECK(ring.size_approx() == 0);
  CHECK(ring.refused() == 0);
}

TEST_CASE("what goes in comes out, in order") {
  small_ring ring;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    CHECK(ring.push(i));
  }
  for (std::uint64_t i = 1; i <= 4; ++i) {
    std::uint64_t out = 0;
    REQUIRE(ring.pop(out));
    CHECK(out == i);
  }
}

TEST_CASE("a full ring refuses rather than overwriting") {
  small_ring ring;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    REQUIRE(ring.push(i));
  }

  // The decision this test exists for. Overwriting would turn a known backlog into a silent hole, and the
  // oldest record is the one whose loss is hardest to notice.
  CHECK_FALSE(ring.push(5));
  CHECK(ring.refused() == 1);

  std::uint64_t out = 0;
  REQUIRE(ring.pop(out));
  CHECK(out == 1);  // still the first one, not the fifth
}

TEST_CASE("space reappears as the consumer drains, and the wrap is invisible") {
  small_ring ring;
  std::uint64_t expected = 1;
  std::uint64_t next = 1;

  // Ten times round a ring of four: if the mask were wrong, or the indices were stored modulo capacity rather
  // than monotonically, this is where it would show.
  for (int cycle = 0; cycle < 10; ++cycle) {
    while (ring.push(next)) {
      ++next;
    }
    std::uint64_t out = 0;
    while (ring.pop(out)) {
      CHECK(out == expected);
      ++expected;
    }
  }
  CHECK(expected == next);
  CHECK(ring.pushed() == next - 1);
  CHECK(ring.popped() == ring.pushed());
}

TEST_CASE("a batch drain takes what is there and no more") {
  small_ring ring;
  REQUIRE(ring.push(10));
  REQUIRE(ring.push(20));

  std::array<std::uint64_t, 8> out{};
  CHECK(ring.pop_batch(out.data(), out.size()) == 2);
  CHECK(out[0] == 10);
  CHECK(out[1] == 20);
  CHECK(ring.pop_batch(out.data(), out.size()) == 0);
}

TEST_CASE("a batch drain honours its limit") {
  small_ring ring;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    REQUIRE(ring.push(i));
  }
  std::array<std::uint64_t, 2> out{};
  CHECK(ring.pop_batch(out.data(), out.size()) == 2);
  CHECK(out[0] == 1);
  CHECK(out[1] == 2);
  CHECK(ring.size_approx() == 2);
}

TEST_CASE("a delivery carries everything the far side needs") {
  const std::byte body[3]{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  conc::delivery record;
  REQUIRE(conc::delivery::from(4711, 2, /*recovered=*/true, dfr::packet_view{body, 3}, record));

  CHECK(record.sequence == 4711);
  CHECK(record.line == 2);
  CHECK(record.recovered);
  CHECK(record.size == 3);
  CHECK(record.payload().u8_at(0) == 'a');
  CHECK(record.payload().u8_at(2) == 'c');
}

TEST_CASE("a body that does not fit is refused, not truncated") {
  std::array<std::byte, conc::kMaxDeliveryBytes + 1> oversized{};
  conc::delivery record;
  // Truncating would hand the consumer a message that decodes into a *plausible* different one, which is worse
  // than handing it nothing.
  CHECK_FALSE(conc::delivery::from(1, 0, false, dfr::packet_view{oversized.data(), oversized.size()},
                                   record));
  CHECK(record.size == 0);
}

TEST_CASE("two threads, and nothing is lost, reordered or repeated") {
  delivery_ring ring;
  constexpr std::uint64_t kMessages = 200'000;

  std::atomic<std::uint64_t> refused{0};

  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kMessages; ++i) {
      conc::delivery record;
      const std::byte body[8]{};
      REQUIRE(conc::delivery::from(i, static_cast<std::uint8_t>(i % 2), i % 3 == 0,
                                   dfr::packet_view{body, 8}, record));
      // Spin rather than drop: this test is about the ring's ordering, and a producer that gave up would make
      // the sequence check vacuous whenever the consumer was slow.
      while (!ring.push(record)) {
        refused.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  std::uint64_t seen = 0;
  std::uint64_t expected = 1;
  bool ordered = true;
  std::thread consumer([&] {
    std::array<conc::delivery, 64> batch{};
    while (seen < kMessages) {
      const std::size_t got = ring.pop_batch(batch.data(), batch.size());
      for (std::size_t i = 0; i < got; ++i) {
        if (batch[i].sequence != expected) {
          ordered = false;
        }
        // Read the body too, so ThreadSanitizer has something to complain about if the release/acquire pair
        // were wrong: a torn record would show as a data race on these bytes rather than on the index.
        if (batch[i].size != 8) {
          ordered = false;
        }
        ++expected;
        ++seen;
      }
    }
  });

  producer.join();
  consumer.join();

  CHECK(ordered);
  CHECK(seen == kMessages);
  CHECK(expected == kMessages + 1);
  CHECK(ring.pushed() == kMessages);
  CHECK(ring.popped() == kMessages);
  // The ring's own refusal counter must agree with what the producer observed, which is the only cross-thread
  // number here that is not a sequence.
  CHECK(ring.refused() == refused.load(std::memory_order_relaxed));
}

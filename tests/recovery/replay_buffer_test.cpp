// The buffer held during recovery: contiguity, two capacities, and refusing to lose data.

#include <dfr/recovery/replay_buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rec = dfr::recovery;

namespace {

// Small on purpose, so both capacity limits are reachable in a test rather than
// theoretical.
using small_buffer = rec::replay_buffer<64, 8>;

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

dfr::packet_view bytes_of(std::string_view text) {
  return dfr::packet_view{text.data(), text.size()};
}

[[nodiscard]] dfr::result<void> put(small_buffer& buffer, std::uint64_t sequence,
                                   std::string_view payload) {
  return buffer.append(sequence, bytes_of(payload));
}

void must_put(small_buffer& buffer, std::uint64_t sequence,
              std::string_view payload) {
  REQUIRE(put(buffer, sequence, payload).has_value());
}

std::vector<std::string> replayed(const small_buffer& buffer) {
  std::vector<std::string> out;
  buffer.replay([&](std::uint64_t, dfr::packet_view message) {
    out.emplace_back(reinterpret_cast<const char*>(message.data()),
                     message.size());
  });
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Appending
// ---------------------------------------------------------------------------

TEST_CASE("a fresh buffer holds nothing", "[recovery][replay_buffer]") {
  const small_buffer buffer;
  CHECK(buffer.empty());
  // NOLINTNEXTLINE(readability-container-size-empty): see schedule_test.cpp's version of this comment: a
  // hand-rolled container's size() and empty() are worth pinning as agreeing, not folded into one check.
  CHECK(buffer.size() == 0);
  CHECK(buffer.bytes_used() == 0);
  CHECK(buffer.buffered().empty());
}

TEST_CASE("the first append sets where the buffer starts",
          "[recovery][replay_buffer]") {
  // It starts wherever the live feed happened to be, which is the number the snapshot is
  // later compared against.
  small_buffer buffer;
  must_put(buffer, 1'000, "a");

  CHECK(buffer.buffered() == range(1'000, 1'001));
  CHECK(buffer.size() == 1);
  CHECK(buffer.bytes_used() == 1);
}

TEST_CASE("consecutive messages extend the buffered range",
          "[recovery][replay_buffer]") {
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  must_put(buffer, 11, "bbb");
  must_put(buffer, 12, "c");

  CHECK(buffer.buffered() == range(10, 13));
  CHECK(buffer.bytes_used() == 6);
  CHECK(replayed(buffer) == std::vector<std::string>{"aa", "bbb", "c"});
}

TEST_CASE("a duplicate is ignored rather than refused",
          "[recovery][replay_buffer]") {
  // Routine during recovery: the second copy from a redundant line, or a retransmit that
  // crossed with the live feed. Refusing would make the caller special-case something
  // that is simply normal.
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  must_put(buffer, 11, "bb");

  must_put(buffer, 10, "aa");
  CHECK(buffer.buffered() == range(10, 12));
  CHECK(buffer.size() == 2);
  CHECK(buffer.bytes_used() == 4);
}

TEST_CASE("a gap is refused, because a replay with a hole is a wrong book",
          "[recovery][replay_buffer]") {
  small_buffer buffer;
  must_put(buffer, 10, "aa");

  const auto skipped = put(buffer, 12, "cc");
  CHECK_FALSE(skipped.has_value());
  CHECK(skipped.error_code() == dfr::error::sequence_gap);
  CHECK(buffer.buffered() == range(10, 11));  // unchanged
}

TEST_CASE("a zero-length message is held as a real entry",
          "[recovery][replay_buffer]") {
  // It occupies a sequence number even though it occupies no bytes, so skipping it would
  // shift every later message by one.
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  must_put(buffer, 11, "");
  must_put(buffer, 12, "cc");

  CHECK(buffer.size() == 3);
  CHECK(buffer.bytes_used() == 4);
  CHECK(replayed(buffer) == std::vector<std::string>{"aa", "", "cc"});
}

// ---------------------------------------------------------------------------
// Two capacities
// ---------------------------------------------------------------------------

TEST_CASE("the message count can run out first",
          "[recovery][replay_buffer]") {
  // A feed of many tiny messages exhausts the index while the arena is nearly empty. An
  // implementation that only checked bytes would keep accepting and write past the index.
  small_buffer buffer;
  for (std::uint64_t i = 0; i < 8; ++i) {
    must_put(buffer, i, "x");
  }
  REQUIRE(buffer.size() == 8);
  REQUIRE(buffer.bytes_used() == 8);  // out of 64: plenty of room left

  const auto refused = put(buffer, 8, "x");
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::recovery_buffer_overflow);
}

TEST_CASE("the byte arena can run out first", "[recovery][replay_buffer]") {
  // And the opposite mix does the opposite. Both limits are real, and which one binds
  // depends on traffic nobody controls.
  small_buffer buffer;
  const std::string big(20, 'x');
  must_put(buffer, 0, big);
  must_put(buffer, 1, big);
  must_put(buffer, 2, big);
  REQUIRE(buffer.size() == 3);
  REQUIRE(buffer.bytes_used() == 60);  // out of 64, with 5 message slots spare

  const auto refused = put(buffer, 3, big);
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error_code() == dfr::error::recovery_buffer_overflow);
}

TEST_CASE("a refused append leaves the buffer exactly as it was",
          "[recovery][replay_buffer][regression]") {
  // The property that makes the refusal usable. A buffer that reported an error while
  // half-writing a message would produce a replay that is wrong in a way no later check
  // can find.
  small_buffer buffer;
  for (std::uint64_t i = 0; i < 8; ++i) {
    must_put(buffer, i, "x");
  }
  const auto before = replayed(buffer);
  const auto range_before = buffer.buffered();

  REQUIRE_FALSE(put(buffer, 8, "y").has_value());

  CHECK(replayed(buffer) == before);
  CHECK(buffer.buffered() == range_before);
  CHECK(buffer.bytes_used() == 8);
}

TEST_CASE("a full buffer refuses instead of overwriting its oldest entry",
          "[recovery][replay_buffer]") {
  // The decision the whole file rests on. A ring buffer would drop message 0 here,
  // silently, and message 0 is exactly the one the snapshot is compared against: losing
  // it is what makes the Glimpse race undetectable.
  small_buffer buffer;
  for (std::uint64_t i = 0; i < 8; ++i) {
    must_put(buffer, i, "x");
  }
  REQUIRE_FALSE(put(buffer, 8, "x").has_value());

  CHECK(buffer.buffered() == range(0, 8));  // still starts at 0
  CHECK(buffer.size() == 8);
}

// ---------------------------------------------------------------------------
// Dropping what a snapshot covers
// ---------------------------------------------------------------------------

TEST_CASE("dropping below a sequence discards the front",
          "[recovery][replay_buffer]") {
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  must_put(buffer, 11, "bbb");
  must_put(buffer, 12, "c");
  must_put(buffer, 13, "dd");

  CHECK(buffer.drop_below(12) == 2);
  CHECK(buffer.buffered() == range(12, 14));
  CHECK(replayed(buffer) == std::vector<std::string>{"c", "dd"});
  CHECK(buffer.bytes_used() == 3);
}

TEST_CASE("dropping compacts the arena so the freed bytes are reusable",
          "[recovery][replay_buffer]") {
  // Otherwise a buffer that had been drained would still report itself full, and a second
  // recovery attempt would fail for no reason.
  small_buffer buffer;
  const std::string big(20, 'x');
  must_put(buffer, 0, big);
  must_put(buffer, 1, big);
  must_put(buffer, 2, big);
  REQUIRE(buffer.bytes_used() == 60);

  REQUIRE(buffer.drop_below(2) == 2);
  CHECK(buffer.bytes_used() == 20);
  must_put(buffer, 3, big);
  must_put(buffer, 4, big);
  CHECK(buffer.size() == 3);
}

TEST_CASE("dropping past the end empties the buffer",
          "[recovery][replay_buffer]") {
  // What happens when the snapshot turns out to cover everything buffered.
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  must_put(buffer, 11, "bb");

  CHECK(buffer.drop_below(500) == 2);
  CHECK(buffer.empty());
  CHECK(buffer.buffered().empty());
}

TEST_CASE("dropping below the start changes nothing",
          "[recovery][replay_buffer]") {
  small_buffer buffer;
  must_put(buffer, 10, "aa");

  CHECK(buffer.drop_below(10) == 0);
  CHECK(buffer.drop_below(5) == 0);
  CHECK(buffer.buffered() == range(10, 11));
}

TEST_CASE("appending continues correctly after a drop",
          "[recovery][replay_buffer]") {
  // The sequence numbering must survive compaction, or the replay would be attributed to
  // the wrong messages.
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  must_put(buffer, 11, "bb");
  REQUIRE(buffer.drop_below(11) == 1);

  must_put(buffer, 12, "cc");
  CHECK(buffer.buffered() == range(11, 13));
  CHECK(replayed(buffer) == std::vector<std::string>{"bb", "cc"});
}

// ---------------------------------------------------------------------------
// Reading back
// ---------------------------------------------------------------------------

TEST_CASE("a message can be fetched by sequence number",
          "[recovery][replay_buffer]") {
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  must_put(buffer, 11, "bbb");

  dfr::packet_view found;
  REQUIRE(buffer.at(11).get(found) == dfr::error::ok);
  CHECK(found.size() == 3);

  CHECK(buffer.at(9).error_code() == dfr::error::invalid_argument);
  CHECK(buffer.at(12).error_code() == dfr::error::invalid_argument);
}

TEST_CASE("replay hands over sequence numbers as well as bytes",
          "[recovery][replay_buffer]") {
  small_buffer buffer;
  must_put(buffer, 100, "a");
  must_put(buffer, 101, "b");
  must_put(buffer, 102, "c");

  std::vector<std::uint64_t> sequences;
  buffer.replay([&](std::uint64_t sequence, dfr::packet_view) {
    sequences.push_back(sequence);
  });
  CHECK(sequences == std::vector<std::uint64_t>{100, 101, 102});
}

TEST_CASE("clearing forgets everything", "[recovery][replay_buffer]") {
  small_buffer buffer;
  must_put(buffer, 10, "aa");
  buffer.clear();

  CHECK(buffer.empty());
  CHECK(buffer.bytes_used() == 0);
  must_put(buffer, 999, "z");  // and can start again anywhere
  CHECK(buffer.buffered() == range(999, 1'000));
}

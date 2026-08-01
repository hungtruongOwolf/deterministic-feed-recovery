// The trace vocabulary and the recorder that stops rather than forgetting.

#include <dfr/trace/recorder.hpp>

#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace trace = dfr::trace;

namespace {

using small_recorder = trace::recorder<4>;

trace::context context_at(std::uint64_t index) {
  return trace::context{.packet_index = index,
                        .time_ns = static_cast<std::int64_t>(index) * 1'000,
                        .client_state = 1,
                        .delivered_before = index * 3,
                        .messages_missing = 0,
                        .outstanding_ranges = 0};
}

}  // namespace

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("every event kind has a distinct name", "[trace]") {
  // So a trace line cannot silently name the wrong thing, and adding a kind without naming it
  // fails to compile rather than writing an empty string into the file.
  std::vector<std::string_view> names;
  for (std::size_t i = 0; i < trace::kEventKindCount; ++i) {
    names.push_back(trace::name_of(static_cast<trace::event_kind>(i)));
  }
  for (const auto& name : names) {
    CHECK_FALSE(name.empty());
  }
  std::vector<std::string_view> sorted = names;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
}

TEST_CASE("every event kind belongs to exactly one layer", "[trace]") {
  // A viewer lanes events by layer, so a kind assigned to none would vanish from the picture and a
  // kind assigned to two would appear twice.
  for (std::size_t i = 0; i < trace::kEventKindCount; ++i) {
    const auto kind = static_cast<trace::event_kind>(i);
    const auto where = trace::layer_of(kind);
    CHECK(static_cast<std::size_t>(where) <
          static_cast<std::size_t>(trace::layer::count_));
    CHECK_FALSE(trace::name_of(where).empty());
  }
}

TEST_CASE("an unnamed layer aborts rather than printing nothing", "[trace]") {
  DFR_CHECK_ABORTS((void)trace::name_of(trace::layer::count_));
}

TEST_CASE("a context stamps the run state onto every event", "[trace]") {
  // The reason context exists: the resulting-state fields are identical for every event recorded
  // at the same moment, and repeating them at twenty call sites is how one ends up stale, which
  // would make the viewer draw a moment that never existed.
  const auto stamped = context_at(7).with(trace::event_kind::gap_opened);
  CHECK(stamped.packet_index == 7);
  CHECK(stamped.time_ns == 7'000);
  CHECK(stamped.kind == trace::event_kind::gap_opened);
  CHECK(stamped.client_state == 1);
  CHECK(stamped.delivered_before == 21);
}

// ---------------------------------------------------------------------------
// The recorder
// ---------------------------------------------------------------------------

TEST_CASE("a fresh recorder holds nothing", "[trace]") {
  const small_recorder recorder;
  CHECK(recorder.empty());
  // NOLINTNEXTLINE(readability-container-size-empty): see schedule_test.cpp's version of this comment: a
  // hand-rolled container's size() and empty() are worth pinning as agreeing, not folded into one check.
  CHECK(recorder.size() == 0);
  CHECK(recorder.dropped() == 0);
  CHECK_FALSE(recorder.full());
}

TEST_CASE("events are kept in the order they happened", "[trace]") {
  small_recorder recorder;
  for (std::uint64_t i = 0; i < 4; ++i) {
    CHECK(recorder.record(context_at(i).with(trace::event_kind::published)));
  }

  REQUIRE(recorder.size() == 4);
  for (std::uint64_t i = 0; i < 4; ++i) {
    CHECK(recorder.at(static_cast<std::size_t>(i)).packet_index == i);
  }
}

TEST_CASE("a full recorder stops and counts what it lost",
          "[trace][regression]") {
  // It does not overwrite the oldest events. The beginning of a trace is where the cause is, and a
  // ring would keep the consequences and discard the explanation: the shape of every unhelpful log
  // file, and the same mistake recovery::replay_buffer refuses to make with data.
  small_recorder recorder;
  for (std::uint64_t i = 0; i < 10; ++i) {
    const bool kept = recorder.record(context_at(i).with(trace::event_kind::published));
    CHECK(kept == (i < 4));
  }

  CHECK(recorder.full());
  CHECK(recorder.size() == 4);
  CHECK(recorder.dropped() == 6);
  CHECK(recorder.at(0).packet_index == 0);  // the beginning survived
  CHECK(recorder.at(3).packet_index == 3);
}

TEST_CASE("record reports whether the event was kept", "[trace]") {
  // So a caller can stop early rather than run a long simulation whose tail it will not see.
  small_recorder recorder;
  for (int i = 0; i < 4; ++i) {
    REQUIRE(recorder.record(context_at(0).with(trace::event_kind::published)));
  }
  CHECK_FALSE(recorder.record(context_at(0).with(trace::event_kind::published)));
}

TEST_CASE("the recorder counts by kind", "[trace]") {
  trace::recorder<16> recorder;
  REQUIRE(recorder.record(context_at(0).with(trace::event_kind::published)));
  REQUIRE(recorder.record(context_at(1).with(trace::event_kind::published)));
  REQUIRE(recorder.record(context_at(2).with(trace::event_kind::gap_opened)));

  const auto counts = recorder.by_kind();
  CHECK(counts[static_cast<std::size_t>(trace::event_kind::published)] == 2);
  CHECK(counts[static_cast<std::size_t>(trace::event_kind::gap_opened)] == 1);
  CHECK(counts[static_cast<std::size_t>(trace::event_kind::state_changed)] == 0);
}

TEST_CASE("reading past the recorded events aborts", "[trace]") {
  DFR_CHECK_ABORTS({
    const small_recorder recorder;
    (void)recorder.at(0);
  });
}

TEST_CASE("clearing resets the loss count too", "[trace]") {
  // Otherwise a second run would inherit the first one's truncation and the viewer would warn about
  // a prefix that was actually complete.
  small_recorder recorder;
  for (int i = 0; i < 10; ++i) {
    (void)recorder.record(context_at(0).with(trace::event_kind::published));
  }
  REQUIRE(recorder.dropped() > 0);

  recorder.clear();
  CHECK(recorder.empty());
  CHECK(recorder.dropped() == 0);
}

TEST_CASE("the recorder is usable at compile time", "[trace]") {
  static_assert([] {
    small_recorder recorder;
    if (!recorder.record(trace::event{.packet_index = 5})) {
      return false;
    }
    return recorder.size() == 1 && recorder.at(0).packet_index == 5;
  }());
  SUCCEED("the static_assert above is the test");
}

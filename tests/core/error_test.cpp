#include <dfr/core/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

namespace {

// Every enumerator except the count_ sentinel, so a test can sweep the whole
// enum rather than a list that drifts from the header.
std::vector<dfr::error> all_errors() {
  std::vector<dfr::error> out;
  out.reserve(dfr::kErrorCount);
  for (std::size_t i = 0; i < dfr::kErrorCount; ++i) {
    out.push_back(static_cast<dfr::error>(i));
  }
  return out;
}

// The classification the header intends, written out independently of the
// range comparisons in is_framing_error and is_sequencing_error. Those helpers
// are implemented as `>=` and `<=` against the first and last enumerator of a
// block, which is fast but silently wrong if anyone reorders the enum. Spelling
// the sets out here is what turns a reorder into a test failure.
const std::set<dfr::error> kFramingErrors{
    dfr::error::truncated_header,        dfr::error::truncated_block,
    dfr::error::block_count_overstated,  dfr::error::block_overruns_datagram,
    dfr::error::trailing_bytes,          dfr::error::unknown_message_type,
    dfr::error::message_length_mismatch,
};

const std::set<dfr::error> kSequencingErrors{
    dfr::error::sequence_gap,    dfr::error::sequence_regressed,
    dfr::error::sequence_reset,  dfr::error::session_changed,
    dfr::error::end_of_session,
};

const std::set<dfr::error> kFatalErrors{
    dfr::error::session_changed,
    dfr::error::snapshot_behind_buffer,
    dfr::error::retransmit_window_exceeded,
    dfr::error::recovery_buffer_overflow,
    dfr::error::lines_diverged,
    dfr::error::end_of_session,
};

}  // namespace

TEST_CASE("ok is zero so `if (err)` reads correctly", "[core][error]") {
  STATIC_REQUIRE(static_cast<std::uint8_t>(dfr::error::ok) == 0);

  // The whole point of ok == 0: a caller can test the code without naming it.
  const dfr::error success = dfr::error::ok;
  const dfr::error failure = dfr::error::sequence_gap;
  CHECK_FALSE(static_cast<bool>(static_cast<std::uint8_t>(success)));
  CHECK(static_cast<bool>(static_cast<std::uint8_t>(failure)));
}

TEST_CASE("the enum fits in a byte", "[core][error]") {
  STATIC_REQUIRE(sizeof(dfr::error) == 1);
  // A byte holds 255 usable codes. If the enum ever outgrows that, the
  // underlying type must change before result<T> grows a padding hole.
  CHECK(dfr::kErrorCount < 256);
}

TEST_CASE("is_fatal marks exactly the codes that invalidate stream state",
          "[core][error]") {
  for (const dfr::error err : all_errors()) {
    const bool expected = kFatalErrors.contains(err);
    CHECK(dfr::is_fatal(err) == expected);
  }

  // ok is never fatal, and count_ is not an error at all.
  CHECK_FALSE(dfr::is_fatal(dfr::error::ok));
  CHECK_FALSE(dfr::is_fatal(dfr::error::count_));
}

TEST_CASE("a sequence gap is not fatal but a session change is",
          "[core][error]") {
  // Stated as its own test because it is the design decision the whole error
  // model turns on: severity is not the axis, recoverability is. A gap loses
  // data and is routine; a session change loses nothing and is terminal.
  CHECK_FALSE(dfr::is_fatal(dfr::error::sequence_gap));
  CHECK_FALSE(dfr::is_fatal(dfr::error::sequence_regressed));
  CHECK(dfr::is_fatal(dfr::error::session_changed));
  CHECK(dfr::is_fatal(dfr::error::snapshot_behind_buffer));
}

TEST_CASE("the range-based classifiers agree with the enum layout",
          "[core][error]") {
  for (const dfr::error err : all_errors()) {
    CHECK(dfr::is_framing_error(err) == kFramingErrors.contains(err));
    CHECK(dfr::is_sequencing_error(err) == kSequencingErrors.contains(err));
  }
}

TEST_CASE("the classifier groups are disjoint", "[core][error]") {
  for (const dfr::error err : all_errors()) {
    const bool framing = dfr::is_framing_error(err);
    const bool sequencing = dfr::is_sequencing_error(err);
    CHECK_FALSE((framing && sequencing));
  }
}

TEST_CASE("every code has a distinct, non-empty name", "[core][error]") {
  std::set<std::string_view> seen;

  for (const dfr::error err : all_errors()) {
    const std::string_view name = dfr::to_string(err);

    CHECK_FALSE(name.empty());
    CHECK(name != "<unknown error>");

    // Names are grepped against this header when reading logs, so a duplicate
    // would make two distinct conditions indistinguishable in output.
    const auto [_, inserted] = seen.insert(name);
    CHECK(inserted);
  }

  CHECK(seen.size() == dfr::kErrorCount);
}

TEST_CASE("every code has a human description", "[core][error]") {
  for (const dfr::error err : all_errors()) {
    const std::string_view text = dfr::describe(err);
    CHECK_FALSE(text.empty());
    CHECK(text != "unknown error");

    // describe() is prose and to_string() is the enumerator spelling. If they
    // are ever equal, one of them has stopped doing its job.
    if (err != dfr::error::ok) {
      CHECK(text != dfr::to_string(err));
    }
  }
}

TEST_CASE("names and descriptions are usable at compile time",
          "[core][error]") {
  // Both are constexpr so that a static_assert can reference them and so that
  // a table of them costs nothing at runtime.
  STATIC_REQUIRE(dfr::to_string(dfr::error::sequence_gap) == "sequence_gap");
  STATIC_REQUIRE(!dfr::describe(dfr::error::sequence_gap).empty());
  STATIC_REQUIRE(dfr::is_fatal(dfr::error::session_changed));
  STATIC_REQUIRE(!dfr::is_fatal(dfr::error::sequence_gap));
}

TEST_CASE("describe() returns a NUL-terminated literal", "[core][error]") {
  // bad_result_access::what() returns describe(...).data() directly, which is
  // only sound because every arm of that switch returns a string literal.
  // Pin it: reading one past the end must give the terminator.
  for (const dfr::error err : all_errors()) {
    const std::string_view text = dfr::describe(err);
    REQUIRE(text.data() != nullptr);
    CHECK(text.data()[text.size()] == '\0');
  }
}

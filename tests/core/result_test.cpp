#include <dfr/core/result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <utility>

namespace {

// A small POD, which is what nearly every T in this library will be.
struct header {
  std::uint64_t sequence{0};
  std::uint16_t block_count{0};

  friend bool operator==(const header&, const header&) = default;
};

// Move-only-ish payload, to prove the accessors move rather than copy.
struct counted {
  int copies{0};
  int moves{0};

  counted() = default;
  counted(const counted& other) : copies(other.copies + 1), moves(other.moves) {}
  counted(counted&& other) noexcept
      : copies(other.copies), moves(other.moves + 1) {}
  // A self-assignment guard would suppress the exact event this type exists to count: it tracks how many
  // times copy-assignment ran, and `x = x` is a real copy-assignment.
  // NOLINTNEXTLINE(cert-oop54-cpp)
  counted& operator=(const counted& other) {
    copies = other.copies + 1;
    moves = other.moves;
    return *this;
  }
  counted& operator=(counted&& other) noexcept {
    copies = other.copies;
    moves = other.moves + 1;
    return *this;
  }
  ~counted() = default;
};

}  // namespace

TEST_CASE("a result built from a value succeeds", "[core][result]") {
  const dfr::result<header> r{header{.sequence = 42, .block_count = 3}};

  CHECK(r.has_value());
  CHECK(static_cast<bool>(r));
  CHECK(r.error_code() == dfr::error::ok);
  CHECK_FALSE(r.is_fatal());
  CHECK(r->sequence == 42);
  CHECK((*r).block_count == 3);
}

TEST_CASE("a result built from an error fails", "[core][result]") {
  const dfr::result<header> r{dfr::error::truncated_header};

  CHECK_FALSE(r.has_value());
  CHECK_FALSE(static_cast<bool>(r));
  CHECK(r.error_code() == dfr::error::truncated_header);
  CHECK_FALSE(r.has_value_and_error());
}

TEST_CASE("a default-constructed result is a failure", "[core][result]") {
  // Defaulting to success would make a forgotten assignment look like a
  // successful decode, which is the worst available failure mode for a
  // library whose entire job is detecting bad input.
  const dfr::result<header> r;

  CHECK_FALSE(r.has_value());
  CHECK(r.error_code() == dfr::error::invalid_argument);
}

TEST_CASE("partial() pairs a usable value with a reported condition",
          "[core][result]") {
  const auto r = dfr::partial(header{.sequence = 7, .block_count = 1},
                              dfr::error::sequence_gap);

  // This is the case a plain optional cannot express: the header parsed fine,
  // and its sequence number revealed that messages were missed.
  CHECK_FALSE(r.has_value());
  CHECK(r.has_value_and_error());
  CHECK(r.error_code() == dfr::error::sequence_gap);
  CHECK(r.value_unsafe().sequence == 7);
}

TEST_CASE("a plain error carries no populated value", "[core][result]") {
  const dfr::result<header> r{dfr::error::truncated_block};

  // The distinction matters: a caller that wants to act on a partial result
  // must be able to tell it apart from a default-constructed placeholder.
  CHECK_FALSE(r.has_value_and_error());
}

TEST_CASE("get() writes the out-param and returns the error",
          "[core][result]") {
  header out{};

  const header expected{.sequence = 99, .block_count = 4};
  const dfr::error err = dfr::result<header>{expected}.get(out);

  CHECK(err == dfr::error::ok);
  CHECK(out == expected);
}

TEST_CASE("get() still reports the error when a value was produced",
          "[core][result]") {
  header out{};

  const dfr::error err =
      dfr::partial(header{.sequence = 5}, dfr::error::sequence_gap).get(out);

  // The value is delivered *and* the condition is returned, so a caller cannot
  // accidentally swallow the gap by only looking at the out-param.
  CHECK(err == dfr::error::sequence_gap);
  CHECK(out.sequence == 5);
}

TEST_CASE("get() on an rvalue moves, on an lvalue copies", "[core][result]") {
  {
    counted out;
    dfr::result<counted>{counted{}}.get(out);
    CHECK(out.moves > 0);
    CHECK(out.copies == 0);
  }
  {
    counted out;
    const dfr::result<counted> r{counted{}};
    r.get(out);
    CHECK(out.copies > 0);
  }
}

TEST_CASE("value_or and error_or supply fallbacks", "[core][result]") {
  const dfr::result<int> good{7};
  const dfr::result<int> bad{dfr::error::truncated_block};

  CHECK(good.value_or(-1) == 7);
  CHECK(bad.value_or(-1) == -1);

  // std::expected's spelling: the error if there is one, otherwise the
  // fallback. Used to collapse a success into a sentinel code.
  CHECK(good.error_or(dfr::error::not_supported) == dfr::error::not_supported);
  CHECK(bad.error_or(dfr::error::not_supported) == dfr::error::truncated_block);
}

TEST_CASE("and_then chains and short-circuits", "[core][result]") {
  const auto double_it = [](int v) { return dfr::result<int>{v * 2}; };

  CHECK(dfr::result<int>{21}.and_then(double_it).value_unsafe() == 42);

  const auto chained =
      dfr::result<int>{dfr::error::truncated_header}.and_then(double_it);
  CHECK_FALSE(chained.has_value());
  CHECK(chained.error_code() == dfr::error::truncated_header);
}

TEST_CASE("transform maps the value and preserves the error",
          "[core][result]") {
  const auto to_text = [](int v) { return std::to_string(v); };

  const auto mapped = dfr::result<int>{5}.transform(to_text);
  STATIC_REQUIRE(std::is_same_v<decltype(mapped), const dfr::result<std::string>>);
  CHECK(mapped.value_unsafe() == "5");

  const auto failed =
      dfr::result<int>{dfr::error::sequence_reset}.transform(to_text);
  CHECK(failed.error_code() == dfr::error::sequence_reset);
}

TEST_CASE("or_else provides a recovery hook", "[core][result]") {
  // This is the shape a recovery path takes: on a gap, substitute a value
  // obtained some other way; on success, pass through untouched.
  const auto recover = [](dfr::error err) {
    return err == dfr::error::sequence_gap ? dfr::result<int>{0}
                                           : dfr::result<int>{err};
  };

  CHECK(dfr::result<int>{9}.or_else(recover).value_unsafe() == 9);
  CHECK(dfr::result<int>{dfr::error::sequence_gap}
            .or_else(recover)
            .value_unsafe() == 0);

  const auto unrecoverable =
      dfr::result<int>{dfr::error::session_changed}.or_else(recover);
  CHECK_FALSE(unrecoverable.has_value());
  CHECK(unrecoverable.error_code() == dfr::error::session_changed);
}

TEST_CASE("transform_error translates a code between layers",
          "[core][result]") {
  const auto escalate = [](dfr::error) { return dfr::error::session_changed; };

  const auto translated =
      dfr::result<int>{dfr::error::sequence_reset}.transform_error(escalate);
  CHECK(translated.error_code() == dfr::error::session_changed);
  CHECK(translated.is_fatal());

  // A success passes through without invoking the mapper.
  CHECK(dfr::result<int>{3}.transform_error(escalate).has_value());
}

TEST_CASE("is_fatal is forwarded from the code", "[core][result]") {
  CHECK(dfr::result<int>{dfr::error::session_changed}.is_fatal());
  CHECK_FALSE(dfr::result<int>{dfr::error::sequence_gap}.is_fatal());
  CHECK_FALSE(dfr::result<int>{1}.is_fatal());
}

TEST_CASE("result<void> defaults to success", "[core][result]") {
  const dfr::result<void> r;

  // Opposite default from result<T>, and deliberately so: there is no value
  // that could have been forgotten, and every call site means "nothing went
  // wrong" by an unassigned result<void>.
  CHECK(r.has_value());
  CHECK(static_cast<bool>(r));
  CHECK(r.error_code() == dfr::error::ok);

  CHECK(dfr::ok().has_value());
  CHECK_FALSE(dfr::result<void>{dfr::error::capacity_exceeded}.has_value());
}

TEST_CASE("result<void> chains", "[core][result]") {
  const auto step = [] { return dfr::result<void>{dfr::error::not_supported}; };

  CHECK(dfr::ok().and_then(step).error_code() == dfr::error::not_supported);
  CHECK(dfr::result<void>{dfr::error::truncated_block}
            .and_then(step)
            .error_code() == dfr::error::truncated_block);

  const auto swallow = [](dfr::error) { return dfr::ok(); };
  CHECK(dfr::result<void>{dfr::error::truncated_block}
            .or_else(swallow)
            .has_value());
}

TEST_CASE("a result over a trivial type stays trivially copyable",
          "[core][result]") {
  // If this ever fails, passing a result by value has stopped being free and
  // the decode path has grown a copy constructor call per message.
  STATIC_REQUIRE(std::is_trivially_copyable_v<dfr::result<header>>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<dfr::result<int>>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<dfr::result<void>>);
}

TEST_CASE("result<void> costs one byte", "[core][result]") {
  STATIC_REQUIRE(sizeof(dfr::result<void>) == 1);
}

TEST_CASE("the populated-value flag packs into existing padding",
          "[core][result]") {
  // header is 16 bytes with 8-byte alignment, so the error byte and the flag
  // byte must both fit in the tail padding rather than growing the type.
  STATIC_REQUIRE(sizeof(header) == 16);
  STATIC_REQUIRE(sizeof(dfr::result<header>) == 24);
}

#if DFR_EXCEPTIONS
TEST_CASE("value() throws and carries the code", "[core][result]") {
  const dfr::result<int> bad{dfr::error::snapshot_behind_buffer};

  CHECK_THROWS_AS(bad.value(), dfr::bad_result_access);

  try {
    // Discarding deliberately: we want the throw, not the value. The cast is
    // required because value() is [[nodiscard]], which is the point.
    static_cast<void>(bad.value());
    FAIL("value() should have thrown");
  } catch (const dfr::bad_result_access& e) {
    CHECK(e.code() == dfr::error::snapshot_behind_buffer);
    CHECK(std::string_view{e.what()} ==
          dfr::describe(dfr::error::snapshot_behind_buffer));
  }

  CHECK(dfr::result<int>{4}.value() == 4);
  CHECK_NOTHROW(dfr::ok().value());
  CHECK_THROWS_AS(dfr::result<void>{dfr::error::not_supported}.value(),
                  dfr::bad_result_access);
}
#endif

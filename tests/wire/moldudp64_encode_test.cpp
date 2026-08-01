#include <dfr/wire/moldudp64.hpp>

#include "support/death_test.hpp"
#include "wire/support/raw_moldudp64.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mold = dfr::wire::moldudp64;
using dfr_test::mold::as_text;

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

TEST_CASE("a default-constructed builder refuses to finish, over an empty buffer",
          "[wire][moldudp64][regression]") {
  // Found alongside message_cursor's version of the same defect (cppcoreguidelines-pro-type-member-init):
  // finish() reads first_sequence_ into a local `header` unconditionally, before encode_header() gets a
  // chance to refuse the empty buffer a default-constructed builder holds, so the read was of an
  // indeterminate value even though the capacity_exceeded this asserts was already the right answer. The
  // fix removes the indeterminate read; this pins the contract the comment above the constructor claims.
  mold::packet_builder builder;
  CHECK_FALSE(builder.finish().has_value());
}

TEST_CASE("a header round-trips through encode and decode",
          "[wire][moldudp64]") {
  mold::session_id session;
  REQUIRE(mold::session_id::from_text("RTRIP").get(session) == dfr::error::ok);

  const mold::header original{
      .session = session, .sequence = 0xDEAD'BEEF'CAFE'BABE, .message_count = 7};

  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  std::size_t written = 0;
  REQUIRE(mold::encode_header(dfr::mutable_packet_view{buffer.data(),
                                                       buffer.size()},
                              original)
              .get(written) == dfr::error::ok);
  CHECK(written == mold::kHeaderSize);

  mold::header decoded;
  REQUIRE(mold::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(decoded) == dfr::error::ok);
  CHECK(decoded == original);
}

TEST_CASE("encoding into too small a buffer fails without writing",
          "[wire][moldudp64]") {
  std::array<std::uint8_t, mold::kHeaderSize - 1> small{};
  const auto outcome = mold::encode_header(
      dfr::mutable_packet_view{small.data(), small.size()}, mold::header{});

  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::capacity_exceeded);
  for (const std::uint8_t b : small) {
    CHECK(b == 0);
  }
}

TEST_CASE("the builder writes the count on finish", "[wire][moldudp64]") {
  // The builder cannot produce a packet whose count disagrees with its contents,
  // because the count is not an input. That defect is one dfr::chaos will inject
  // deliberately, so the honest encoder must be incapable of it by accident.
  mold::session_id session;
  REQUIRE(mold::session_id::from_text("BUILD").get(session) == dfr::error::ok);

  std::array<std::uint8_t, 128> buffer{};
  mold::packet_builder builder{
      *mold::packet_builder::into(
          dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 400)};

  CHECK(builder.message_count() == 0);
  CHECK(builder.size() == mold::kHeaderSize);

  const std::array<std::string_view, 3> bodies{"alpha", "b", "gamma!"};
  for (const std::string_view body : bodies) {
    REQUIRE(builder.append(dfr::packet_view{body.data(), body.size()})
                .has_value());
  }
  CHECK(builder.message_count() == 3);

  dfr::packet_view finished;
  REQUIRE(builder.finish().get(finished) == dfr::error::ok);

  // Decode what was built and check every field survived.
  mold::message_cursor cursor;
  REQUIRE(mold::message_cursor::over(finished).get(cursor) == dfr::error::ok);
  CHECK(cursor.packet_header().session == session);
  CHECK(cursor.packet_header().sequence == 400);
  CHECK(cursor.packet_header().message_count == 3);
  CHECK(cursor.packet_header().next_sequence() == 403);

  std::vector<std::string_view> seen;
  std::vector<std::uint64_t> sequences;
  REQUIRE(cursor
              .drain([&](const mold::message& m) {
                seen.push_back(as_text(m.payload));
                sequences.push_back(m.sequence);
              })
              .has_value());

  CHECK(seen == std::vector<std::string_view>{"alpha", "b", "gamma!"});
  CHECK(sequences == std::vector<std::uint64_t>{400, 401, 402});
}

TEST_CASE("the builder refuses a message it cannot fit",
          "[wire][moldudp64]") {
  const mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize + 4> buffer{};
  mold::packet_builder builder{
      *mold::packet_builder::into(
          dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 1)};

  // Two bytes of length plus two of payload exactly fills it.
  REQUIRE(builder.append(dfr::packet_view{"ab", 2}).has_value());
  CHECK(builder.size() == buffer.size());

  const auto rejected = builder.append(dfr::packet_view{"c", 1});
  CHECK_FALSE(rejected.has_value());
  CHECK(rejected.error_code() == dfr::error::capacity_exceeded);

  // And the rejection left the packet intact, so finish() still works.
  CHECK(builder.message_count() == 1);
  CHECK(builder.finish().has_value());
}

TEST_CASE("a heartbeat and an end-of-session encode to header-only packets",
          "[wire][moldudp64]") {
  const mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  const dfr::mutable_packet_view out{buffer.data(), buffer.size()};

  REQUIRE(mold::encode_heartbeat(out, session, 777).has_value());
  mold::header beat;
  REQUIRE(mold::decode_header(out.as_const()).get(beat) == dfr::error::ok);
  CHECK(beat.kind() == mold::packet_kind::heartbeat);
  CHECK(beat.sequence == 777);
  CHECK(beat.next_sequence() == 777);

  REQUIRE(mold::encode_end_of_session(out, session, 888).has_value());
  mold::header last;
  REQUIRE(mold::decode_header(out.as_const()).get(last) == dfr::error::ok);
  CHECK(last.kind() == mold::packet_kind::end_of_session);
  CHECK(last.sequence == 888);
}

// ---------------------------------------------------------------------------
// Retransmission requests
// ---------------------------------------------------------------------------

TEST_CASE("a request count is clamped to the protocol maximum",
          "[wire][moldudp64]") {
  // The facility rejects an over-large request outright rather than truncating
  // it, so a client that asks for a million messages receives nothing and then
  // reports a timeout it caused itself.
  STATIC_REQUIRE(mold::clamp_request_count(1) == 1);
  STATIC_REQUIRE(mold::clamp_request_count(60'000) == 60'000);
  STATIC_REQUIRE(mold::clamp_request_count(60'001) == 60'000);
  STATIC_REQUIRE(mold::clamp_request_count(1'000'000) == 60'000);
  STATIC_REQUIRE(mold::clamp_request_count(UINT64_MAX) == 60'000);

  // And the clamp must never land on the end-of-session sentinel.
  STATIC_REQUIRE(mold::clamp_request_count(UINT64_MAX) != mold::kEndOfSession);
}

TEST_CASE("a request encodes as a header with the wanted range",
          "[wire][moldudp64]") {
  mold::session_id session;
  REQUIRE(mold::session_id::from_text("REQ").get(session) == dfr::error::ok);

  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  REQUIRE(mold::encode_request(
              dfr::mutable_packet_view{buffer.data(), buffer.size()}, session,
              1000, 250)
              .has_value());

  mold::header request;
  REQUIRE(mold::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(request) == dfr::error::ok);
  CHECK(request.session == session);
  CHECK(request.sequence == 1000);
  CHECK(request.message_count == 250);
}

TEST_CASE("a request is clamped when encoded", "[wire][moldudp64]") {
  const mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  REQUIRE(mold::encode_request(
              dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 1,
              5'000'000)
              .has_value());

  mold::header request;
  REQUIRE(mold::decode_header(dfr::packet_view{buffer.data(), buffer.size()})
              .get(request) == dfr::error::ok);
  CHECK(request.message_count == mold::kMaxMessagesPerRequest);
}

TEST_CASE("a zero-count request is rejected", "[wire][moldudp64]") {
  // Asking for nothing is always a caller bug rather than a protocol state, and
  // on the wire it would be indistinguishable from a heartbeat.
  const mold::session_id session;
  std::array<std::uint8_t, mold::kHeaderSize> buffer{};
  const auto outcome = mold::encode_request(
      dfr::mutable_packet_view{buffer.data(), buffer.size()}, session, 1, 0);

  CHECK_FALSE(outcome.has_value());
  CHECK(outcome.error_code() == dfr::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// Compile-time use
// ---------------------------------------------------------------------------

TEST_CASE("a header decodes at compile time", "[wire][moldudp64]") {
  // Which means a wire-format expectation can be pinned in a static_assert,
  // with no fixture and no runtime at all.
  static constexpr std::array<std::byte, mold::kHeaderSize> kBytes{
      std::byte{'S'},  std::byte{' '},  std::byte{' '},  std::byte{' '},
      std::byte{' '},  std::byte{' '},  std::byte{' '},  std::byte{' '},
      std::byte{' '},  std::byte{' '},  std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x64}, std::byte{0x00}, std::byte{0x03}};

  static constexpr dfr::packet_view kPacket{kBytes.data(), kBytes.size()};
  constexpr auto kDecoded = mold::decode_header(kPacket);

  STATIC_REQUIRE(kDecoded.has_value());
  STATIC_REQUIRE(kDecoded.value_unsafe().sequence == 100);
  STATIC_REQUIRE(kDecoded.value_unsafe().message_count == 3);
  STATIC_REQUIRE(kDecoded.value_unsafe().next_sequence() == 103);
  STATIC_REQUIRE(kDecoded.value_unsafe().kind() == mold::packet_kind::data);
}

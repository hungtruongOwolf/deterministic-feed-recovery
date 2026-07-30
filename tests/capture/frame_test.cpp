#include <dfr/capture/frame.hpp>

#include "capture/support/eth_frame.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace cap = dfr::capture;

TEST_CASE("a frame reports truncation", "[capture][frame]") {
  const std::array<std::uint8_t, 64> stored{};
  const dfr::packet_view data{stored.data(), stored.size()};

  // A capture taken with a snaplen shorter than the frame. Detecting it is what
  // stops a capture artefact being reported as a malformed packet: the publisher
  // sent 1500 bytes and the file holds 64.
  const cap::frame cut{.timestamp_ns = 1, .data = data, .wire_length = 1500};
  CHECK(cut.truncated());

  const cap::frame whole{.timestamp_ns = 1, .data = data, .wire_length = 64};
  CHECK_FALSE(whole.truncated());
}

TEST_CASE("a wire length below the stored length is still not truncation",
          "[capture][frame]") {
  // Should not happen in a well-formed file, and the accessor answers the
  // question it is asked rather than trying to detect the file being wrong.
  // Whether that inconsistency is worth reporting belongs to the reader, which
  // has the block context to say so.
  const std::array<std::uint8_t, 64> stored{};
  const cap::frame odd{.timestamp_ns = 0,
                       .data = {stored.data(), stored.size()},
                       .wire_length = 32};
  CHECK_FALSE(odd.truncated());
}

TEST_CASE("a default frame is empty and untruncated", "[capture][frame]") {
  const cap::frame empty;
  CHECK(empty.data.empty());
  CHECK(empty.wire_length == 0);
  CHECK_FALSE(empty.truncated());
  CHECK(empty.timestamp_ns == 0);
}

TEST_CASE("parse_udp reads a frame's link layer", "[capture][frame]") {
  // The convenience overload exists so a caller does not have to remember that
  // frame::data is the link layer rather than the payload.
  const auto built = dfr_test::eth::iex_shaped("via-frame");
  const cap::frame captured{.timestamp_ns = 123,
                            .data = built.view(),
                            .wire_length =
                                static_cast<std::uint32_t>(built.size())};

  cap::udp_datagram datagram;
  REQUIRE(cap::parse_udp(captured).get(datagram) == dfr::error::ok);
  CHECK(dfr_test::eth::as_text(datagram.payload) == "via-frame");
  CHECK(datagram.vlan_id == 1013);
}

TEST_CASE("the only named link type is Ethernet", "[capture][frame]") {
  // Anything else is reported by the readers rather than guessed at, because a
  // capture on a Linux cooked socket or a raw IP interface has a different
  // header and would decode as nonsense.
  STATIC_REQUIRE(static_cast<std::uint16_t>(cap::link_type::ethernet) == 1);
}

TEST_CASE("a timestamp is a bare integer, not a clock time point",
          "[capture][frame]") {
  // Deliberate. It comes from the capturing host's clock — a different machine,
  // with its own offset and drift — so the type system should refuse to compare
  // it against a local time point without an explicit correction.
  STATIC_REQUIRE(
      std::is_same_v<decltype(cap::frame{}.timestamp_ns), std::uint64_t>);
}

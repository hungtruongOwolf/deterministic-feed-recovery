#include <dfr/capture/ethernet.hpp>

#include "capture/support/eth_frame.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

namespace cap = dfr::capture;
using dfr_test::eth::as_text;
using dfr_test::eth::frame_builder;
using dfr_test::eth::iex_shaped;

TEST_CASE("an untagged frame yields its UDP payload", "[capture][ethernet]") {
  const auto f = frame_builder{}
                     .ethertype(cap::kEtherTypeIpv4)
                     .ipv4()
                     .udp(1234, 5678, "hello")
                     .seal();

  cap::udp_datagram datagram;
  REQUIRE(cap::parse_ethernet_udp(f.view()).get(datagram) == dfr::error::ok);

  CHECK(datagram.source_port == 1234);
  CHECK(datagram.destination_port == 5678);
  CHECK(as_text(datagram.payload) == "hello");
  CHECK(datagram.vlan_tags == 0);
  CHECK(datagram.vlan_id == 0);
}

TEST_CASE("a VLAN-tagged frame still parses, and reports the tag",
          "[capture][ethernet][regression]") {
  // The test this file exists for. IEX HIST captures carry VLAN 1013. A reader
  // that assumes the EtherType is at offset 12 finds 0x8100 there, does not
  // recognise it as IPv4, and reports that the file contains no IP traffic at
  // all: silently, and for every packet.
  const auto f = iex_shaped("payload");

  cap::udp_datagram datagram;
  REQUIRE(cap::parse_ethernet_udp(f.view()).get(datagram) == dfr::error::ok);

  CHECK(datagram.vlan_tags == 1);
  CHECK(datagram.vlan_id == 1013);
  CHECK(as_text(datagram.payload) == "payload");
  CHECK(datagram.destination_port == 10378);
  CHECK(datagram.multicast());
}

TEST_CASE("only the low twelve bits of the TCI are the VLAN id",
          "[capture][ethernet]") {
  // The other four are priority and the drop-eligible flag. Masking the whole
  // 16 bits gives a plausible but wrong id, which then makes a per-VLAN filter
  // match nothing.
  const auto f = frame_builder{}
                     .vlan(0xE000 | 42)  // priority 7, id 42
                     .ethertype(cap::kEtherTypeIpv4)
                     .ipv4()
                     .udp(1, 2, "x")
                     .seal();

  cap::udp_datagram datagram;
  REQUIRE(cap::parse_ethernet_udp(f.view()).get(datagram) == dfr::error::ok);
  CHECK(datagram.vlan_id == 42);
}

TEST_CASE("stacked QinQ tags are walked", "[capture][ethernet]") {
  const auto f = frame_builder{}
                     .vlan(100, cap::kEtherTypeQinQ)
                     .vlan(200)
                     .ethertype(cap::kEtherTypeIpv4)
                     .ipv4()
                     .udp(1, 2, "deep")
                     .seal();

  cap::udp_datagram datagram;
  REQUIRE(cap::parse_ethernet_udp(f.view()).get(datagram) == dfr::error::ok);

  CHECK(datagram.vlan_tags == 2);
  // The outer tag identifies the frame for a filtering caller.
  CHECK(datagram.vlan_id == 100);
  CHECK(as_text(datagram.payload) == "deep");
}

TEST_CASE("the tag walk is bounded", "[capture][ethernet]") {
  // TIGER_STYLE: put a limit on everything. Without one, a crafted frame can
  // present an unbounded chain of 0x8100 EtherTypes.
  const auto f = frame_builder{}
                     .vlan(1)
                     .vlan(2)
                     .vlan(3)
                     .ethertype(cap::kEtherTypeIpv4)
                     .ipv4()
                     .udp(1, 2, "x")
                     .seal();

  const auto parsed = cap::parse_ethernet_udp(f.view());
  CHECK_FALSE(parsed.has_value());
  CHECK(parsed.error_code() == dfr::error::not_supported);
}

TEST_CASE("a non-IPv4 frame is reported, not an error in the file",
          "[capture][ethernet]") {
  // ARP, IPv6 and LLDP all legitimately appear in a capture, so the caller
  // decides whether to skip or to complain.
  for (const std::uint16_t type : {std::uint16_t{0x0806},   // ARP
                                   std::uint16_t{0x86DD},   // IPv6
                                   std::uint16_t{0x88CC}}) {  // LLDP
    const auto f = frame_builder{}.ethertype(type).udp(1, 2, "x");
    const auto parsed = cap::parse_ethernet_udp(f.view());
    CHECK_FALSE(parsed.has_value());
    CHECK(parsed.error_code() == dfr::error::not_supported);
  }
}

TEST_CASE("IPv4 options shift the payload", "[capture][ethernet]") {
  // IHL counts 32-bit words, so a header may carry options past the 20-byte
  // minimum. Ignoring IHL means reading the options as the UDP header.
  const auto f = frame_builder{}
                     .ethertype(cap::kEtherTypeIpv4)
                     .ipv4(cap::kIpProtocolUdp, /*ihl_words=*/7)
                     .udp(4321, 8765, "after-options")
                     .seal();

  cap::udp_datagram datagram;
  REQUIRE(cap::parse_ethernet_udp(f.view()).get(datagram) == dfr::error::ok);
  CHECK(datagram.source_port == 4321);
  CHECK(as_text(datagram.payload) == "after-options");
}

TEST_CASE("a fragmented datagram is refused", "[capture][ethernet]") {
  // A fragment cannot be decoded from one frame. Refusing is what turns the MTU
  // war story into a reported condition instead of days of confusion: a few
  // extra bytes once fragmented every market-data message on a real feed.
  const auto more_fragments = frame_builder{}
                                  .ethertype(cap::kEtherTypeIpv4)
                                  .ipv4(cap::kIpProtocolUdp, 5, 0x2000)
                                  .udp(1, 2, "first-part")
                                  .seal();
  CHECK(cap::parse_ethernet_udp(more_fragments.view()).error_code() ==
        dfr::error::not_supported);

  const auto later_fragment = frame_builder{}
                                  .ethertype(cap::kEtherTypeIpv4)
                                  .ipv4(cap::kIpProtocolUdp, 5, 0x0001)
                                  .udp(1, 2, "later-part")
                                  .seal();
  CHECK(cap::parse_ethernet_udp(later_fragment.view()).error_code() ==
        dfr::error::not_supported);
}

TEST_CASE("a non-UDP protocol is refused", "[capture][ethernet]") {
  const auto f = frame_builder{}
                     .ethertype(cap::kEtherTypeIpv4)
                     .ipv4(/*protocol=*/6)  // TCP
                     .udp(1, 2, "x")
                     .seal();

  CHECK(cap::parse_ethernet_udp(f.view()).error_code() ==
        dfr::error::not_supported);
}

TEST_CASE("Ethernet padding is not part of the payload",
          "[capture][ethernet][regression]") {
  // A switch pads any frame below 60 bytes. Bounding the payload by the frame
  // length instead of the UDP length would hand those zero bytes to the protocol
  // decoder, which then reports trailing bytes that the publisher never sent.
  const auto f = iex_shaped("ab").pad_to(60);
  REQUIRE(f.size() == 60);

  cap::udp_datagram datagram;
  REQUIRE(cap::parse_ethernet_udp(f.view()).get(datagram) == dfr::error::ok);
  CHECK(datagram.payload.size() == 2);
  CHECK(as_text(datagram.payload) == "ab");
}

TEST_CASE("a snaplen truncation is reported", "[capture][ethernet]") {
  // The UDP length says more follows than the capture stored. Reporting it is
  // what stops a capture artefact being attributed to the publisher.
  const auto f = iex_shaped("this payload is long enough to cut").truncate_to(50);

  const auto parsed = cap::parse_ethernet_udp(f.view());
  CHECK_FALSE(parsed.has_value());
  CHECK(parsed.error_code() == dfr::error::truncated_block);
}

TEST_CASE("truncation at each layer is reported", "[capture][ethernet]") {
  const auto full = iex_shaped("payload");

  // Every prefix shorter than the full frame must produce an error rather than a
  // read past the end. Sweeping every length is cheap and covers each layer's
  // boundary without hand-picking them.
  for (std::size_t cut = 0; cut < full.size(); ++cut) {
    const auto partial = iex_shaped("payload").truncate_to(cut);
    const auto parsed = cap::parse_ethernet_udp(partial.view());
    CHECK_FALSE(parsed.has_value());
  }

  CHECK(cap::parse_ethernet_udp(full.view()).has_value());
}

TEST_CASE("a UDP length below its own header is refused",
          "[capture][ethernet]") {
  const auto f = frame_builder{}
                     .ethertype(cap::kEtherTypeIpv4)
                     .ipv4()
                     .udp(1, 2, "x", /*declared_length=*/4)
                     .seal();

  CHECK(cap::parse_ethernet_udp(f.view()).error_code() ==
        dfr::error::message_length_mismatch);
}

TEST_CASE("multicast is recognised", "[capture][ethernet]") {
  // A market-data capture with no multicast in it is almost certainly the wrong
  // file or the wrong interface, so this is worth being able to assert.
  const auto group = frame_builder{}
                         .ethertype(cap::kEtherTypeIpv4)
                         .ipv4(cap::kIpProtocolUdp, 5, 0, 0x0A000001, 0xE9D71504)
                         .udp(1, 2, "x")
                         .seal();
  cap::udp_datagram datagram;
  REQUIRE(cap::parse_ethernet_udp(group.view()).get(datagram) == dfr::error::ok);
  CHECK(datagram.multicast());
  CHECK(datagram.destination_address == 0xE9D71504);  // 233.215.21.4, IEX DEEP

  const auto unicast = frame_builder{}
                           .ethertype(cap::kEtherTypeIpv4)
                           .ipv4(cap::kIpProtocolUdp, 5, 0, 0x0A000001, 0x0A000002)
                           .udp(1, 2, "x")
                           .seal();
  REQUIRE(cap::parse_ethernet_udp(unicast.view()).get(datagram) ==
          dfr::error::ok);
  CHECK_FALSE(datagram.multicast());
}

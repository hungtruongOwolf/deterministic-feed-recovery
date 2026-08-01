// Extracting a UDP payload from a captured Ethernet frame.
//
// The one thing here that is not boilerplate, and the reason this file exists as
// a named step rather than an inline offset calculation:
//
//   **The 802.1Q VLAN tag.** IEX HIST captures carry one (VLAN 1013 in the 2017
//   files). A reader that assumes the EtherType sits at offset 12 finds 0x8100
//   there, does not recognise it as IPv4, and parses nothing at all, while
//   reporting no error, because from its point of view the file simply contains
//   no IP traffic. That failure is silent and total, and it is the single most
//   likely reason a first attempt at parsing a real capture yields zero packets.
//
// So the EtherType walk is explicit, handles stacked tags, and reports the VLAN
// it found rather than discarding it.

#ifndef DFR_CAPTURE_ETHERNET_HPP
#define DFR_CAPTURE_ETHERNET_HPP

#include <dfr/capture/frame.hpp>
#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace capture {

// ---------------------------------------------------------------------------
// Layer constants
// ---------------------------------------------------------------------------

inline constexpr std::size_t kEthernetHeaderSize = 14;  // dst, src, ethertype

// An 802.1Q tag is four bytes: a two-byte TPID followed by a two-byte TCI.
// Recorded for reference, but it is deliberately NOT the number the walk below
// consumes: see kVlanTciAndTypeSize.
inline constexpr std::size_t kVlanTagSize = 4;

// What the walk actually steps over per tag.
//
// The TPID occupies the EtherType position and has therefore already been read
// as part of the 14-byte Ethernet header. What remains for one tag is the
// two-byte TCI plus the two-byte EtherType that follows it. Conflating this with
// kVlanTagSize is an off-by-two that shifts every subsequent layer, and it reads
// plausibly enough to survive review.
inline constexpr std::size_t kVlanTciAndTypeSize = 4;
inline constexpr std::size_t kIpv4MinHeaderSize = 20;
inline constexpr std::size_t kUdpHeaderSize = 8;

inline constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr std::uint16_t kEtherTypeVlan = 0x8100;      // 802.1Q
inline constexpr std::uint16_t kEtherTypeQinQ = 0x88A8;      // 802.1ad outer
inline constexpr std::uint8_t kIpProtocolUdp = 17;

// How many stacked VLAN tags to walk before giving up.
//
// A bound rather than a loop to exhaustion, per TIGER_STYLE's "put a limit on
// everything": a crafted frame could otherwise present an unbounded chain of
// 0x8100 EtherTypes. Two covers 802.1ad QinQ, which is the deepest anything real
// uses.
inline constexpr int kMaxVlanTags = 2;

// The 12 bits that actually identify the VLAN; the other four in the TCI are
// priority and the drop-eligible flag.
inline constexpr std::uint16_t kVlanIdMask = 0x0FFF;

// ---------------------------------------------------------------------------
// udp_datagram
// ---------------------------------------------------------------------------

struct udp_datagram {
  std::uint32_t source_address{0};
  std::uint32_t destination_address{0};
  std::uint16_t source_port{0};
  std::uint16_t destination_port{0};

  // Zero when the frame carried no VLAN tag. Kept rather than discarded because
  // a capture that unexpectedly has, or lacks, a tag is worth knowing about,
  // and because a multi-VLAN capture may need filtering by it.
  std::uint16_t vlan_id{0};
  int vlan_tags{0};

  packet_view payload{};

  [[nodiscard]] constexpr bool multicast() const noexcept {
    // 224.0.0.0/4. Named because a market-data capture that contains no
    // multicast is almost certainly the wrong file or the wrong interface.
    return (destination_address >> 28) == 0xE;
  }

  [[nodiscard]] friend bool operator==(const udp_datagram&,
                                       const udp_datagram&) = default;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// Ethernet -> optional VLAN tags -> IPv4 -> UDP.
//
// Every layer is sliced through packet_view::subview, so a truncated capture
// produces an error rather than a read past the end. That matters more here than
// in the wire decoders: capture files are routinely truncated by a snaplen, so
// short frames are the normal case rather than the hostile one.
[[nodiscard]] constexpr result<udp_datagram> parse_ethernet_udp(
    packet_view eth_frame) noexcept {
  udp_datagram out;

  packet_view cursor = eth_frame;
  packet_view header;
  if (const auto err = cursor.prefix(kEthernetHeaderSize).get(header);
      err != error::ok) DFR_UNLIKELY {
    return error::truncated_header;
  }

  std::uint16_t ether_type = header.be16_at(12);
  if (const auto err = cursor.consume(kEthernetHeaderSize); !err) DFR_UNLIKELY {
    return err.error_code();
  }

  // Walk any VLAN tags. The EtherType field of a tagged frame holds the TPID,
  // and the real type follows the two-byte TCI.
  while (ether_type == kEtherTypeVlan || ether_type == kEtherTypeQinQ) {
    if (out.vlan_tags >= kMaxVlanTags) DFR_UNLIKELY {
      return error::not_supported;
    }

    packet_view tag;
    if (const auto err = cursor.prefix(kVlanTciAndTypeSize).get(tag);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }
    const std::uint16_t tci = tag.be16_at(0);
    if (out.vlan_tags == 0) {
      out.vlan_id = static_cast<std::uint16_t>(tci & kVlanIdMask);
    }
    ether_type = tag.be16_at(2);
    ++out.vlan_tags;

    if (const auto err = cursor.consume(kVlanTciAndTypeSize); !err) DFR_UNLIKELY {
      return err.error_code();
    }
  }

  if (ether_type != kEtherTypeIpv4) DFR_UNLIKELY {
    // Not an error in the file: ARP, IPv6 and LLDP all legitimately appear in a
    // capture. The caller decides whether to skip or to complain.
    return error::not_supported;
  }

  packet_view ip;
  if (const auto err = cursor.prefix(kIpv4MinHeaderSize).get(ip);
      err != error::ok) DFR_UNLIKELY {
    return error::truncated_header;
  }

  const std::uint8_t version_and_ihl = ip.u8_at(0);
  if ((version_and_ihl >> 4) != 4) DFR_UNLIKELY {
    return error::not_supported;
  }
  // IHL counts 32-bit words, so a header may carry options beyond the 20-byte
  // minimum. Ignoring it is how a decoder ends up reading options as UDP.
  const std::size_t ip_header_size =
      static_cast<std::size_t>(version_and_ihl & 0x0F) * 4;
  if (ip_header_size < kIpv4MinHeaderSize) DFR_UNLIKELY {
    return error::truncated_header;
  }

  // A fragmented datagram cannot be decoded from one frame. Reported rather than
  // silently parsed, because the MTU war story in BUILD-GUIDE.md is exactly
  // this: a few extra bytes fragmented every market-data message, and it took
  // days to find.
  const std::uint16_t flags_and_offset = ip.be16_at(6);
  const bool more_fragments = (flags_and_offset & 0x2000) != 0;
  const std::uint16_t fragment_offset = flags_and_offset & 0x1FFF;
  if (more_fragments || fragment_offset != 0) DFR_UNLIKELY {
    return error::not_supported;
  }

  if (ip.u8_at(9) != kIpProtocolUdp) DFR_UNLIKELY {
    return error::not_supported;
  }
  out.source_address = ip.be32_at(12);
  out.destination_address = ip.be32_at(16);

  // The IP total length bounds the payload independently of the frame length.
  // Trusting the frame instead would include Ethernet padding, which a switch
  // adds to any frame below 60 bytes, and that padding would then be decoded as
  // trailing protocol bytes.
  const std::uint16_t ip_total_length = ip.be16_at(2);
  if (ip_total_length < ip_header_size) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }

  if (const auto err = cursor.consume(ip_header_size); !err) DFR_UNLIKELY {
    return error::truncated_header;
  }

  packet_view udp;
  if (const auto err = cursor.prefix(kUdpHeaderSize).get(udp);
      err != error::ok) DFR_UNLIKELY {
    return error::truncated_header;
  }
  out.source_port = udp.be16_at(0);
  out.destination_port = udp.be16_at(2);

  // The UDP length includes its own 8-byte header.
  const std::uint16_t udp_length = udp.be16_at(4);
  if (udp_length < kUdpHeaderSize) DFR_UNLIKELY {
    return error::message_length_mismatch;
  }
  const std::size_t payload_length = udp_length - kUdpHeaderSize;

  if (const auto err = cursor.consume(kUdpHeaderSize); !err) DFR_UNLIKELY {
    return err.error_code();
  }
  if (const auto err = cursor.prefix(payload_length).get(out.payload);
      err != error::ok) DFR_UNLIKELY {
    // The declared length exceeds what the capture holds: a snaplen truncation.
    return error::truncated_block;
  }

  return out;
}

// The same, from a capture record. A convenience so a caller does not have to
// remember that a frame's data is the link layer.
[[nodiscard]] constexpr result<udp_datagram> parse_udp(
    const frame& captured) noexcept {
  return parse_ethernet_udp(captured.data);
}

}  // namespace capture
}  // namespace dfr::inline v1

#endif  // DFR_CAPTURE_ETHERNET_HPP

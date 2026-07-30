// A hand-assembled Ethernet/IPv4/UDP frame builder, for tests only.
//
// Every layer's length fields can be set independently of what actually follows,
// so a test can produce the truncations and inconsistencies a real capture
// contains and the demultiplexer must refuse.

#ifndef DFR_TESTS_CAPTURE_SUPPORT_ETH_FRAME_HPP
#define DFR_TESTS_CAPTURE_SUPPORT_ETH_FRAME_HPP

#include <dfr/capture/ethernet.hpp>
#include <dfr/core/packet_view.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dfr_test::eth {

namespace cap = dfr::capture;

class frame_builder {
 public:
  // Destination and source MAC. Content is irrelevant to the demultiplexer, so
  // they are fixed rather than parameterised.
  frame_builder() {
    for (int i = 0; i < 6; ++i) {
      bytes_.push_back(static_cast<std::uint8_t>(0x10 + i));  // dst
    }
    for (int i = 0; i < 6; ++i) {
      bytes_.push_back(static_cast<std::uint8_t>(0x20 + i));  // src
    }
  }

  // Adds an 802.1Q tag. Call before ethertype(); the TPID goes where the
  // EtherType would have been, which is the whole reason a naive reader fails.
  frame_builder& vlan(std::uint16_t id, std::uint16_t tpid = cap::kEtherTypeVlan) {
    push_be16(tpid);
    push_be16(id);  // TCI: priority and DEI left zero
    return *this;
  }

  frame_builder& ethertype(std::uint16_t type) {
    push_be16(type);
    return *this;
  }

  // An IPv4 header. `ihl_words` above 5 appends that many words of options, so a
  // test can check the payload offset honours IHL.
  frame_builder& ipv4(std::uint8_t protocol = cap::kIpProtocolUdp,
                      std::uint8_t ihl_words = 5, std::uint16_t flags_offset = 0,
                      std::uint32_t src = 0x0A000001,
                      std::uint32_t dst = 0xE9D71504) {
    ip_start_ = bytes_.size();
    bytes_.push_back(static_cast<std::uint8_t>(0x40 | ihl_words));
    bytes_.push_back(0);      // DSCP/ECN
    push_be16(0);             // total length, patched by seal()
    push_be16(0x1234);        // identification
    push_be16(flags_offset);
    bytes_.push_back(64);     // TTL
    bytes_.push_back(protocol);
    push_be16(0);             // checksum, never verified here
    push_be32(src);
    push_be32(dst);
    for (int i = 5; i < ihl_words; ++i) {
      push_be32(0x01010100);  // NOP options
    }
    return *this;
  }

  // A UDP header. `declared_length` overrides the value written into the length
  // field, so a test can make it disagree with the payload.
  frame_builder& udp(std::uint16_t src_port, std::uint16_t dst_port,
                     std::string_view payload,
                     int declared_length = -1) {
    push_be16(src_port);
    push_be16(dst_port);
    const std::size_t length = declared_length >= 0
                                   ? static_cast<std::size_t>(declared_length)
                                   : cap::kUdpHeaderSize + payload.size();
    push_be16(static_cast<std::uint16_t>(length));
    push_be16(0);  // checksum
    for (const char c : payload) {
      bytes_.push_back(static_cast<std::uint8_t>(c));
    }
    return *this;
  }

  // Patches the IPv4 total length to match what was actually appended, which is
  // what a truthful sender would write.
  frame_builder& seal() {
    if (ip_start_ != kAbsent) {
      const auto total = static_cast<std::uint16_t>(bytes_.size() - ip_start_);
      bytes_[ip_start_ + 2] = static_cast<std::uint8_t>(total >> 8);
      bytes_[ip_start_ + 3] = static_cast<std::uint8_t>(total & 0xFF);
    }
    return *this;
  }

  // Appends Ethernet padding, as a switch does for any frame below 60 bytes.
  // The payload must still come out at the UDP length, not the frame remainder.
  frame_builder& pad_to(std::size_t total) {
    while (bytes_.size() < total) {
      bytes_.push_back(0x00);
    }
    return *this;
  }

  // Cuts the frame short, as a capture snaplen does.
  frame_builder& truncate_to(std::size_t total) {
    if (bytes_.size() > total) {
      bytes_.resize(total);
    }
    return *this;
  }

  [[nodiscard]] dfr::packet_view view() const {
    return {bytes_.data(), bytes_.size()};
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

 private:
  static constexpr std::size_t kAbsent = static_cast<std::size_t>(-1);

  void push_be16(std::uint16_t v) {
    bytes_.push_back(static_cast<std::uint8_t>(v >> 8));
    bytes_.push_back(static_cast<std::uint8_t>(v & 0xFF));
  }
  void push_be32(std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      bytes_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
  }

  std::vector<std::uint8_t> bytes_;
  std::size_t ip_start_{kAbsent};
};

// The shape a real IEX HIST frame has: one VLAN tag, IPv4, UDP to a multicast
// group. Named so the tests read as statements about that file rather than about
// abstract framing.
inline frame_builder iex_shaped(std::string_view payload,
                                std::uint16_t vlan_id = 1013) {
  frame_builder f;
  f.vlan(vlan_id)
      .ethertype(cap::kEtherTypeIpv4)
      .ipv4()
      .udp(10378, 10378, payload)
      .seal();
  return f;
}

inline std::string_view as_text(dfr::packet_view v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

}  // namespace dfr_test::eth

#endif  // DFR_TESTS_CAPTURE_SUPPORT_ETH_FRAME_HPP

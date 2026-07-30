// A hand-assembled MoldUDP64 datagram builder, for tests only.
//
// Shared rather than copied into each test file, per docs/STYLE.md section 1.10.
// Its whole purpose is to produce packets the library's own encoder is
// deliberately incapable of producing — an overstated block count, a lying
// length field — so that the decoder's refusals can be tested at all.

#ifndef DFR_TESTS_WIRE_SUPPORT_RAW_MOLDUDP64_HPP
#define DFR_TESTS_WIRE_SUPPORT_RAW_MOLDUDP64_HPP

#include <dfr/core/packet_view.hpp>
#include <dfr/wire/moldudp64/constants.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dfr_test::mold {

namespace wire = dfr::wire::moldudp64;

// Builds a datagram byte by byte, so a test can produce malformed packets that
// the library's own encoder is designed to be incapable of producing.
class raw_packet {
 public:
  raw_packet& session(std::string_view name) {
    for (std::size_t i = 0; i < wire::kSessionSize; ++i) {
      bytes_.push_back(i < name.size() ? static_cast<std::uint8_t>(name[i])
                                       : std::uint8_t{' '});
    }
    return *this;
  }

  raw_packet& sequence(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
    return *this;
  }

  raw_packet& count(std::uint16_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    return *this;
  }

  // A well-formed block: a truthful length followed by that many bytes.
  raw_packet& block(std::string_view payload) {
    return declared_block(static_cast<std::uint16_t>(payload.size()), payload);
  }

  // A block whose declared length need not match what follows, which is how the
  // second half of the helix defect is reproduced.
  raw_packet& declared_block(std::uint16_t declared, std::string_view payload) {
    bytes_.push_back(static_cast<std::uint8_t>(declared >> 8));
    bytes_.push_back(static_cast<std::uint8_t>(declared & 0xFF));
    for (const char c : payload) {
      bytes_.push_back(static_cast<std::uint8_t>(c));
    }
    return *this;
  }

  raw_packet& raw_byte(std::uint8_t value) {
    bytes_.push_back(value);
    return *this;
  }

  [[nodiscard]] dfr::packet_view view() const {
    return {bytes_.data(), bytes_.size()};
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

inline std::string_view as_text(dfr::packet_view v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

// A well-formed three-message packet at sequence 100.
inline raw_packet three_messages() {
  return raw_packet{}
      .session("SESS01")
      .sequence(100)
      .count(3)
      .block("aaa")
      .block("bb")
      .block("c");
}


}  // namespace dfr_test::mold

#endif  // DFR_TESTS_WIRE_SUPPORT_RAW_MOLDUDP64_HPP

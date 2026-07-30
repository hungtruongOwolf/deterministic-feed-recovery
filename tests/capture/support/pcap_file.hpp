// A hand-assembled classic pcap file, for tests only.
//
// Writes the file header and record headers in whichever byte order the test
// asks for, and lets every length field be set independently of what follows, so
// the truncations and inconsistencies a real capture contains can be produced.

#ifndef DFR_TESTS_CAPTURE_SUPPORT_PCAP_FILE_HPP
#define DFR_TESTS_CAPTURE_SUPPORT_PCAP_FILE_HPP

#include <dfr/capture/pcap.hpp>
#include <dfr/core/packet_view.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dfr_test::pcap {

namespace cap = dfr::capture::pcap;

class file_builder {
 public:
  explicit file_builder(
      std::array<std::uint8_t, 4> magic = cap::kMagicLittleMicros,
      std::uint32_t snaplen = 65535, std::uint32_t link = 1)
      : little_(magic == cap::kMagicLittleMicros ||
                magic == cap::kMagicLittleNanos) {
    for (const std::uint8_t b : magic) {
      bytes_.push_back(b);
    }
    push16(2);         // version major
    push16(4);         // version minor
    push32(0);         // thiszone
    push32(0);         // sigfigs
    push32(snaplen);
    push32(link);
  }

  // One record. `declared_captured` and `declared_original` override the length
  // fields so a test can make them disagree with the payload.
  file_builder& record(std::uint32_t seconds, std::uint32_t fraction,
                       std::string_view payload,
                       long long declared_captured = -1,
                       long long declared_original = -1) {
    push32(seconds);
    push32(fraction);
    push32(declared_captured >= 0
               ? static_cast<std::uint32_t>(declared_captured)
               : static_cast<std::uint32_t>(payload.size()));
    push32(declared_original >= 0
               ? static_cast<std::uint32_t>(declared_original)
               : static_cast<std::uint32_t>(payload.size()));
    for (const char c : payload) {
      bytes_.push_back(static_cast<std::uint8_t>(c));
    }
    return *this;
  }

  file_builder& raw_byte(std::uint8_t v) {
    bytes_.push_back(v);
    return *this;
  }

  // Cuts the file short, as a full disk or an interrupted capture does.
  file_builder& truncate_to(std::size_t total) {
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
  void push16(std::uint16_t v) {
    if (little_) {
      bytes_.push_back(static_cast<std::uint8_t>(v & 0xFF));
      bytes_.push_back(static_cast<std::uint8_t>(v >> 8));
    } else {
      bytes_.push_back(static_cast<std::uint8_t>(v >> 8));
      bytes_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
  }
  void push32(std::uint32_t v) {
    if (little_) {
      for (int shift = 0; shift <= 24; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
      }
    } else {
      for (int shift = 24; shift >= 0; shift -= 8) {
        bytes_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
      }
    }
  }

  std::vector<std::uint8_t> bytes_;
  bool little_;
};

inline std::string_view as_text(dfr::packet_view v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

}  // namespace dfr_test::pcap

#endif  // DFR_TESTS_CAPTURE_SUPPORT_PCAP_FILE_HPP

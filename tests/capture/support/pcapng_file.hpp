// A hand-assembled pcapng file, for tests only.
//
// Writes blocks in either byte order and lets each length field be overridden, so
// the malformed envelopes a reader must refuse: a trailing length that disagrees,
// a length below the framing size, one that is not a multiple of four: can all be
// produced. The library's own code has no way to emit those.

#ifndef DFR_TESTS_CAPTURE_SUPPORT_PCAPNG_FILE_HPP
#define DFR_TESTS_CAPTURE_SUPPORT_PCAPNG_FILE_HPP

#include <dfr/capture/pcapng.hpp>
#include <dfr/core/packet_view.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dfr_test::pcapng {

namespace ng = dfr::capture::pcapng;

class file_builder {
 public:
  explicit file_builder(bool little = true) : little_(little) {}

  // A Section Header Block. Every file must start with one, and each one resets
  // the reader's interface table.
  file_builder& section(std::uint16_t major = 1, std::uint16_t minor = 0) {
    std::vector<std::uint8_t> body;
    push32_into(body, ng::kByteOrderMagic);
    push16_into(body, major);
    push16_into(body, minor);
    for (int i = 0; i < 8; ++i) {
      body.push_back(0xFF);  // section length: -1, unknown
    }
    return block(ng::kSectionHeaderBlock, body);
  }

  // An Interface Description Block. `tsresol` below zero omits the option, which
  // is what most real files do and which must mean microseconds.
  file_builder& interface_description(std::uint16_t link = 1,
                                      std::uint32_t snaplen = 65535,
                                      int tsresol = -1) {
    std::vector<std::uint8_t> body;
    push16_into(body, link);
    push16_into(body, 0);  // reserved
    push32_into(body, snaplen);
    if (tsresol >= 0) {
      push16_into(body, ng::kOptionIfTsResolution);
      push16_into(body, 1);
      body.push_back(static_cast<std::uint8_t>(tsresol));
      body.push_back(0);  // padding to a 4-byte boundary
      body.push_back(0);
      body.push_back(0);
      push16_into(body, ng::kOptionEndOfOpt);
      push16_into(body, 0);
    }
    return block(ng::kInterfaceDescriptionBlock, body);
  }

  // An Enhanced Packet Block. `declared_captured` and `declared_original`
  // override the length fields so they can be made to disagree with the data.
  file_builder& packet(std::uint64_t ticks, std::string_view data,
                       std::uint32_t interface_id = 0,
                       long long declared_captured = -1,
                       long long declared_original = -1) {
    std::vector<std::uint8_t> body;
    push32_into(body, interface_id);
    push32_into(body, static_cast<std::uint32_t>(ticks >> 32));
    push32_into(body, static_cast<std::uint32_t>(ticks & 0xFFFF'FFFF));
    push32_into(body, declared_captured >= 0
                          ? static_cast<std::uint32_t>(declared_captured)
                          : static_cast<std::uint32_t>(data.size()));
    push32_into(body, declared_original >= 0
                          ? static_cast<std::uint32_t>(declared_original)
                          : static_cast<std::uint32_t>(data.size()));
    for (const char c : data) {
      body.push_back(static_cast<std::uint8_t>(c));
    }
    while ((body.size() % 4) != 0) {
      body.push_back(0);  // the format pads packet data; the padding is not data
    }
    return block(ng::kEnhancedPacketBlock, body);
  }

  // A Simple Packet Block: no timestamp, no interface id.
  file_builder& simple_packet(std::string_view data) {
    std::vector<std::uint8_t> body;
    push32_into(body, static_cast<std::uint32_t>(data.size()));
    for (const char c : data) {
      body.push_back(static_cast<std::uint8_t>(c));
    }
    while ((body.size() % 4) != 0) {
      body.push_back(0);
    }
    return block(ng::kSimplePacketBlock, body);
  }

  // A block of a type the reader has never heard of, which it must skip.
  file_builder& unknown_block(std::uint32_t type = 0x0000'0BAD) {
    return block(type, std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
  }

  // One block, with optional overrides of the two length copies.
  file_builder& block(std::uint32_t type, const std::vector<std::uint8_t>& body,
                      long long declared_leading = -1,
                      long long declared_trailing = -1) {
    const auto total = static_cast<std::uint32_t>(body.size() + ng::kBlockFrameSize);
    push32(type);
    push32(declared_leading >= 0 ? static_cast<std::uint32_t>(declared_leading)
                                 : total);
    for (const std::uint8_t b : body) {
      bytes_.push_back(b);
    }
    push32(declared_trailing >= 0 ? static_cast<std::uint32_t>(declared_trailing)
                                  : total);
    return *this;
  }

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
  void push16(std::uint16_t v) { push16_into(bytes_, v); }
  void push32(std::uint32_t v) { push32_into(bytes_, v); }

  void push16_into(std::vector<std::uint8_t>& out, std::uint16_t v) const {
    if (little_) {
      out.push_back(static_cast<std::uint8_t>(v & 0xFF));
      out.push_back(static_cast<std::uint8_t>(v >> 8));
    } else {
      out.push_back(static_cast<std::uint8_t>(v >> 8));
      out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
  }
  void push32_into(std::vector<std::uint8_t>& out, std::uint32_t v) const {
    if (little_) {
      for (int shift = 0; shift <= 24; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
      }
    } else {
      for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
      }
    }
  }

  std::vector<std::uint8_t> bytes_;
  bool little_;
};

// A minimal well-formed file: one section, one Ethernet interface, one packet.
inline file_builder minimal(std::string_view payload, bool little = true,
                            int tsresol = -1) {
  file_builder f{little};
  f.section().interface_description(1, 65535, tsresol).packet(0, payload);
  return f;
}

inline std::string_view as_text(dfr::packet_view v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

}  // namespace dfr_test::pcapng

#endif  // DFR_TESTS_CAPTURE_SUPPORT_PCAPNG_FILE_HPP

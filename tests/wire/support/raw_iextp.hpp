// A hand-assembled IEX-TP datagram builder, for tests only.
//
// Shared rather than copied, per docs/STYLE.md section 1.10. Its purpose is to
// produce packets the library's own encoder is deliberately incapable of
// producing: a payload length that disagrees with the blocks, an unknown
// transport version, so that the decoder's refusals can be tested at all.

#ifndef DFR_TESTS_WIRE_SUPPORT_RAW_IEXTP_HPP
#define DFR_TESTS_WIRE_SUPPORT_RAW_IEXTP_HPP

#include <dfr/core/packet_view.hpp>
#include <dfr/wire/iextp/constants.hpp>
#include <dfr/wire/iextp/header.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dfr_test::iex {

namespace wire = dfr::wire::iextp;

// Assembles a datagram byte by byte, little-endian, so a test can produce
// packets the library's own encoder is designed to be unable to produce.
class raw_packet {
 public:
  raw_packet() {
    // Version 1, reserved 0. Overridable via version().
    bytes_.assign(wire::kHeaderSize, 0);
    bytes_[wire::kVersionOffset] = wire::kVersion;
  }

  raw_packet& version(std::uint8_t v) {
    bytes_[wire::kVersionOffset] = v;
    return *this;
  }
  raw_packet& protocol(std::uint16_t v) { return put_le(wire::kProtocolIdOffset, v); }
  raw_packet& channel(std::uint32_t v) { return put_le(wire::kChannelIdOffset, v); }
  raw_packet& session(std::uint32_t v) { return put_le(wire::kSessionIdOffset, v); }
  raw_packet& payload_length(std::uint16_t v) {
    return put_le(wire::kPayloadLengthOffset, v);
  }
  raw_packet& count(std::uint16_t v) { return put_le(wire::kMessageCountOffset, v); }
  raw_packet& stream_offset(std::int64_t v) {
    return put_le(wire::kStreamOffsetOffset, static_cast<std::uint64_t>(v));
  }
  raw_packet& first_sequence(std::uint64_t v) {
    return put_le(wire::kFirstSequenceOffset, v);
  }
  raw_packet& send_time(std::uint64_t v) { return put_le(wire::kSendTimeOffset, v); }

  // A truthful block: its declared length matches what follows.
  raw_packet& block(std::string_view payload) {
    return declared_block(static_cast<std::uint16_t>(payload.size()), payload);
  }

  raw_packet& declared_block(std::uint16_t declared, std::string_view payload) {
    bytes_.push_back(static_cast<std::uint8_t>(declared & 0xFF));
    bytes_.push_back(static_cast<std::uint8_t>(declared >> 8));
    for (const char c : payload) {
      bytes_.push_back(static_cast<std::uint8_t>(c));
    }
    return *this;
  }

  raw_packet& raw_byte(std::uint8_t v) {
    bytes_.push_back(v);
    return *this;
  }

  // Sets Payload Length to whatever was actually appended, which is what a
  // truthful publisher would do.
  raw_packet& seal() {
    return payload_length(
        static_cast<std::uint16_t>(bytes_.size() - wire::kHeaderSize));
  }

  [[nodiscard]] dfr::packet_view view() const {
    return {bytes_.data(), bytes_.size()};
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

 private:
  template <typename T>
  raw_packet& put_le(std::size_t at, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes_[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return *this;
  }

  std::vector<std::uint8_t> bytes_;
};

inline std::string_view as_text(dfr::packet_view v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

inline wire::header prototype() {
  return wire::header{.protocol = static_cast<std::uint16_t>(wire::protocol_id::deep),
                     .channel = 1,
                     .session = 0xABCD,
                     .stream_offset = 0,
                     .first_sequence = 1,
                     .send_time_ns = 1'700'000'000'000'000'000ULL};
}


}  // namespace dfr_test::iex

#endif  // DFR_TESTS_WIRE_SUPPORT_RAW_IEXTP_HPP

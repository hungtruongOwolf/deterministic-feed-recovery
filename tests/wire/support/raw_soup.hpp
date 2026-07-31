// Building SoupBinTCP bytes by hand, so a decoder test does not depend on the encoder.
//
// The two are checked against each other elsewhere. Here the bytes are laid out literally, because a
// decoder test written against its own encoder agrees with itself about a misreading of the
// specification — which is precisely the mistake the length field invites.

#ifndef DFR_TESTS_WIRE_SUPPORT_RAW_SOUP_HPP
#define DFR_TESTS_WIRE_SUPPORT_RAW_SOUP_HPP

#include <dfr/core/packet_view.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dfr_test::soup {

class raw_stream {
 public:
  // Appends a frame, computing the length field the way the specification states it: the type byte
  // counts, the length field does not count itself.
  raw_stream& frame(char type, std::string_view payload) {
    const std::uint16_t declared = static_cast<std::uint16_t>(1 + payload.size());
    bytes_.push_back(static_cast<char>((declared >> 8) & 0xFF));
    bytes_.push_back(static_cast<char>(declared & 0xFF));
    bytes_.push_back(type);
    bytes_.append(payload);
    return *this;
  }

  // A frame whose declared length disagrees with what follows, for the tests that need a lie.
  raw_stream& declared_frame(std::uint16_t declared, char type,
                             std::string_view payload) {
    bytes_.push_back(static_cast<char>((declared >> 8) & 0xFF));
    bytes_.push_back(static_cast<char>(declared & 0xFF));
    bytes_.push_back(type);
    bytes_.append(payload);
    return *this;
  }

  raw_stream& raw(std::string_view literal) {
    bytes_.append(literal);
    return *this;
  }

  // Login Accepted's payload: session left-justified in ten bytes, sequence right-justified in
  // twenty. Two conventions, adjacent, spelled out here so a reader can see them.
  raw_stream& login_accepted(std::string_view session, std::string_view sequence) {
    std::string payload;
    payload = std::string{session};
    payload.resize(10, ' ');
    std::string right(20, ' ');
    right.replace(20 - sequence.size(), sequence.size(), sequence);
    return frame('A', payload + right);
  }

  [[nodiscard]] dfr::packet_view view() const {
    return {bytes_.data(), bytes_.size()};
  }
  [[nodiscard]] dfr::packet_view from(std::size_t at) const {
    return {bytes_.data() + at, bytes_.size() - at};
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

  // A prefix, for the tests about a packet that has not finished arriving.
  [[nodiscard]] dfr::packet_view prefix(std::size_t count) const {
    return {bytes_.data(), count};
  }

 private:
  std::string bytes_;
};

}  // namespace dfr_test::soup

#endif  // DFR_TESTS_WIRE_SUPPORT_RAW_SOUP_HPP

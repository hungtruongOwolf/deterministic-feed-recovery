// Walking pcapng block frames.
//
// Block framing is independent of what the blocks mean, so it is its own type:
// this validates the envelope and hands back a body, and reader.hpp decides what
// the body is.
//
// The bootstrap that makes the format readable at all lives here too. A Section
// Header Block's own Block Total Length is written in the section's byte order,
// which is not known until the byte-order magic inside its body has been read.
// The format solves this by giving the SHB a palindromic type, 0x0A0D0D0A, so it
// can be recognised before the order is known.

#ifndef DFR_CAPTURE_PCAPNG_BLOCK_HPP
#define DFR_CAPTURE_PCAPNG_BLOCK_HPP

#include <dfr/capture/pcapng/constants.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::capture::pcapng {

class block_walker {
 public:
  constexpr block_walker() noexcept = default;
  explicit constexpr block_walker(packet_view file) noexcept : rest_(file) {}

  [[nodiscard]] constexpr bool done() const noexcept { return rest_.empty(); }
  [[nodiscard]] constexpr std::size_t remaining() const noexcept {
    return rest_.size();
  }
  [[nodiscard]] constexpr packet_view position() const noexcept { return rest_; }
  constexpr void rewind_to(packet_view where) noexcept { rest_ = where; }
  [[nodiscard]] constexpr bool little_endian() const noexcept { return little_; }
  constexpr void set_little_endian(bool little) noexcept { little_ = little; }

  // Reads one block frame, validating both length copies, and leaves the cursor
  // after it. `body` excludes the twelve bytes of framing.
  [[nodiscard]] constexpr result<void> take(std::uint32_t& type,
                                            packet_view& body) noexcept {
    packet_view head;
    if (const auto err = rest_.prefix(kBlockHeaderSize).get(head);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }
    type = read32(head, 0);
    const std::uint32_t total = read32(head, 4);

    // Validated before it is used to advance. A length below the framing size, or
    // one that is not a multiple of four, would otherwise loop forever or land
    // mid-field.
    if (total < kBlockFrameSize || (total % 4) != 0) DFR_UNLIKELY {
      return error::message_length_mismatch;
    }

    packet_view whole;
    if (const auto err = rest_.prefix(total).get(whole); err != error::ok)
        DFR_UNLIKELY {
      return error::truncated_block;
    }

    // The trailing copy of the length is a free integrity check the format hands
    // us. Ignoring it accepts a corrupted length and then walks to a wrong offset
    // for every block after this one.
    if (read32(whole, total - 4) != total) DFR_UNLIKELY {
      return error::message_length_mismatch;
    }

    if (const auto err =
            whole.subview(kBlockHeaderSize, total - kBlockFrameSize).get(body);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_block;
    }
    return rest_.consume(total);
  }

  // The block type at the cursor, read order-independently. Only valid for
  // recognising a Section Header Block, whose type is palindromic.
  [[nodiscard]] constexpr result<std::uint32_t> peek_type() const noexcept {
    packet_view head;
    if (const auto err = rest_.prefix(4).get(head); err != error::ok)
        DFR_UNLIKELY {
      return error::truncated_header;
    }
    return head.be32_at(0);
  }

  [[nodiscard]] constexpr result<packet_view> peek_at(
      std::size_t offset, std::size_t length) const noexcept {
    return rest_.subview(offset, length);
  }

  [[nodiscard]] constexpr std::uint16_t read16(packet_view v,
                                               std::size_t at) const noexcept {
    return little_ ? v.le16_at(at) : v.be16_at(at);
  }
  [[nodiscard]] constexpr std::uint32_t read32(packet_view v,
                                               std::size_t at) const noexcept {
    return little_ ? v.le32_at(at) : v.be32_at(at);
  }

 private:
  packet_view rest_;
  bool little_{true};
};

}  // namespace dfr::inline v1::capture::pcapng
#endif  // DFR_CAPTURE_PCAPNG_BLOCK_HPP

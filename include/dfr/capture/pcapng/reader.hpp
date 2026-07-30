// Reading frames out of a pcapng file held in memory.
//
// Same contract as the classic reader: the caller supplies the bytes, nothing is
// opened here, and a failed next() consumes nothing so that done() means "every
// byte belonged to a complete block".
//
// Three things make this harder than classic pcap, and all three appear in the
// IEX HIST corpus:
//
//   1. **The byte order is per section, and bootstrapped.** See block.hpp.
//   2. **Timestamp units come from the interface, not the packet.** See
//      timestamp.hpp. Two channels in one file may legitimately differ.
//   3. **A merged file has several sections.** IEX HIST files carry an SHB
//      comment reading "File created by merging: File1: ...". Each new section
//      resets the interface table, so an Enhanced Packet Block's interface id is
//      only meaningful relative to its own section. Carrying interfaces across a
//      section boundary silently attributes packets to the wrong link type and
//      the wrong timestamp resolution.

#ifndef DFR_CAPTURE_PCAPNG_READER_HPP
#define DFR_CAPTURE_PCAPNG_READER_HPP

#include <dfr/capture/frame.hpp>
#include <dfr/capture/pcapng/block.hpp>
#include <dfr/capture/pcapng/constants.hpp>
#include <dfr/capture/pcapng/options.hpp>
#include <dfr/capture/pcapng/timestamp.hpp>
#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace capture::pcapng {

struct interface_info {
  std::uint16_t link{0};
  std::uint32_t snaplen{0};
  tick_resolution resolution{};

  [[nodiscard]] constexpr bool is_ethernet() const noexcept {
    return link == static_cast<std::uint16_t>(link_type::ethernet);
  }
};

class reader {
 public:
  constexpr reader() noexcept = default;

  // The first block must be a Section Header Block; the format requires it, and a
  // file that does not begin with one is not pcapng.
  [[nodiscard]] static constexpr result<reader> over(packet_view file) noexcept {
    reader out;
    out.blocks_ = block_walker{file};
    if (const auto err = out.read_section_header(); !err) DFR_UNLIKELY {
      return err.error_code();
    }
    return out;
  }

  [[nodiscard]] constexpr bool done() const noexcept { return blocks_.done(); }
  [[nodiscard]] constexpr std::size_t remaining() const noexcept {
    return blocks_.remaining();
  }
  [[nodiscard]] constexpr std::uint64_t frames_read() const noexcept {
    return frames_read_;
  }
  [[nodiscard]] constexpr std::uint64_t blocks_skipped() const noexcept {
    return blocks_skipped_;
  }
  [[nodiscard]] constexpr std::uint32_t sections() const noexcept {
    return sections_;
  }
  [[nodiscard]] constexpr std::size_t interfaces() const noexcept {
    return interface_count_;
  }
  [[nodiscard]] constexpr const interface_info* interface_at(
      std::size_t index) const noexcept {
    return index < interface_count_ ? &interfaces_[index] : nullptr;
  }

  // Advances to the next packet block, handling and skipping everything else.
  //
  // Returns end_of_session when the remaining blocks were all metadata, so a
  // caller looping on !done() gets a clean end rather than an error.
  //
  // The loop terminates because every iteration consumes at least
  // kBlockFrameSize bytes and the length is validated before use, which is what
  // turns a zero or malformed length into a reported error rather than a hang.
  [[nodiscard]] constexpr result<frame> next() noexcept {
    DFR_ASSERT(!done(), "next() on an exhausted reader; check done() first");

    while (!blocks_.done()) {
      const packet_view at_block_start = blocks_.position();

      std::uint32_t type = 0;
      packet_view body;
      if (const auto err = blocks_.take(type, body); !err) DFR_UNLIKELY {
        blocks_.rewind_to(at_block_start);
        return err.error_code();
      }

      switch (type) {
        case kSectionHeaderBlock: {
          // Rewind so read_section_header sees the whole block: it must re-read
          // the length in the byte order it is about to discover.
          blocks_.rewind_to(at_block_start);
          if (const auto err = read_section_header(); !err) DFR_UNLIKELY {
            blocks_.rewind_to(at_block_start);
            return err.error_code();
          }
          break;
        }
        case kInterfaceDescriptionBlock: {
          if (const auto err = add_interface(body); !err) DFR_UNLIKELY {
            blocks_.rewind_to(at_block_start);
            return err.error_code();
          }
          break;
        }
        case kEnhancedPacketBlock: {
          frame out;
          if (const auto err = decode_enhanced_packet(body).get(out);
              err != error::ok) DFR_UNLIKELY {
            blocks_.rewind_to(at_block_start);
            return err;
          }
          ++frames_read_;
          return out;
        }
        case kSimplePacketBlock: {
          // A Simple Packet Block carries no timestamp and no interface id.
          // Reported rather than surfaced with a fabricated timestamp of zero,
          // which would silently corrupt any ordering the caller derives.
          blocks_.rewind_to(at_block_start);
          return error::not_supported;
        }
        default:
          // Unknown blocks are skipped by design; tolerating them is the whole
          // reason the format carries a length. take() already consumed it.
          ++blocks_skipped_;
          break;
      }
    }

    return error::end_of_session;
  }

  template <typename Handler>
  [[nodiscard]] constexpr result<void> drain(Handler&& handler) noexcept {
    while (!done()) {
      frame current;
      const auto err = next().get(current);
      if (err == error::end_of_session) {
        break;  // trailing metadata only
      }
      if (err != error::ok) DFR_UNLIKELY {
        return err;
      }
      handler(current);
    }
    return ok();
  }

 private:
  // Reads a Section Header Block from the current position, establishing the byte
  // order before anything else is interpreted.
  [[nodiscard]] constexpr result<void> read_section_header() noexcept {
    std::uint32_t peeked = 0;
    if (const auto err = blocks_.peek_type().get(peeked); err != error::ok)
        DFR_UNLIKELY {
      return err;
    }
    // Valid in either order because the SHB type is palindromic — the bootstrap
    // that lets the byte order be discovered at all.
    if (peeked != kSectionHeaderBlock) DFR_UNLIKELY {
      return error::not_supported;
    }

    packet_view magic_field;
    if (const auto err =
            blocks_.peek_at(kBlockHeaderSize + kShbByteOrderMagicOffset, 4)
                .get(magic_field);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }
    if (magic_field.le32_at(0) == kByteOrderMagic) {
      blocks_.set_little_endian(true);
    } else if (magic_field.be32_at(0) == kByteOrderMagic) {
      blocks_.set_little_endian(false);
    } else DFR_UNLIKELY {
      return error::not_supported;
    }

    // Only now can the length be read in the right order.
    std::uint32_t type = 0;
    packet_view body;
    if (const auto err = blocks_.take(type, body); !err) DFR_UNLIKELY {
      return err.error_code();
    }
    DFR_ASSERT(type == kSectionHeaderBlock,
               "read_section_header consumed a block that is not an SHB");

    if (body.size() < kShbBodyMinSize) DFR_UNLIKELY {
      return error::truncated_block;
    }
    if (blocks_.read16(body, kShbMajorOffset) != kSupportedMajor) DFR_UNLIKELY {
      return error::not_supported;
    }

    // A new section invalidates every interface id. Carrying them across would
    // attribute packets to the wrong link type and the wrong timestamp
    // resolution, and merged IEX HIST files really do contain several sections.
    interface_count_ = 0;
    ++sections_;
    return ok();
  }

  [[nodiscard]] constexpr result<void> add_interface(packet_view body) noexcept {
    if (body.size() < kIdbBodyMinSize) DFR_UNLIKELY {
      return error::truncated_block;
    }
    if (interface_count_ >= kMaxInterfaces) DFR_UNLIKELY {
      return error::capacity_exceeded;
    }

    interface_info info;
    info.link = blocks_.read16(body, kIdbLinkTypeOffset);
    info.snaplen = blocks_.read32(body, kIdbSnaplenOffset);

    packet_view options;
    if (body.suffix(kIdbBodyMinSize).get(options) == error::ok) {
      std::uint8_t raw = tick_resolution::kDefaultRaw;
      if (find_timestamp_resolution(options, blocks_.little_endian(), raw)) {
        info.resolution = tick_resolution{raw};
      }
    }

    interfaces_[interface_count_] = info;
    ++interface_count_;
    return ok();
  }

  [[nodiscard]] constexpr result<frame> decode_enhanced_packet(
      packet_view body) const noexcept {
    if (body.size() < kEpbBodyMinSize) DFR_UNLIKELY {
      return error::truncated_block;
    }

    const std::uint32_t interface_id = blocks_.read32(body, kEpbInterfaceOffset);
    const std::uint32_t high = blocks_.read32(body, kEpbTimestampHighOffset);
    const std::uint32_t low = blocks_.read32(body, kEpbTimestampLowOffset);
    const std::uint32_t captured = blocks_.read32(body, kEpbCapturedOffset);
    const std::uint32_t original = blocks_.read32(body, kEpbOriginalOffset);

    if (interface_id >= interface_count_) DFR_UNLIKELY {
      // A packet naming an interface this section never described. Reported
      // rather than defaulted to interface 0, because guessing would attach the
      // wrong timestamp resolution to real data.
      return error::invalid_argument;
    }
    if (captured > original) DFR_UNLIKELY {
      return error::message_length_mismatch;
    }

    packet_view data;
    if (const auto err = body.subview(kEpbBodyMinSize, captured).get(data);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_block;
    }

    const std::uint64_t ticks = (static_cast<std::uint64_t>(high) << 32) |
                                static_cast<std::uint64_t>(low);
    return frame{
        .timestamp_ns =
            interfaces_[interface_id].resolution.to_nanoseconds(ticks),
        .data = data,
        .wire_length = original,
    };
  }

  block_walker blocks_;
  std::array<interface_info, kMaxInterfaces> interfaces_{};
  std::size_t interface_count_{0};
  std::uint64_t frames_read_{0};
  std::uint64_t blocks_skipped_{0};
  std::uint32_t sections_{0};
};

}  // namespace capture::pcapng
}  // namespace dfr::inline v1

#endif  // DFR_CAPTURE_PCAPNG_READER_HPP

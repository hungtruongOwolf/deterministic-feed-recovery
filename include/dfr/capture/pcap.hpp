// Classic libpcap capture files.
//
// Layout:
//
//   File header, 24 bytes
//     0   4  magic          determines BOTH byte order and timestamp resolution
//     4   2  version major  2
//     6   2  version minor  4
//     8   4  thiszone       GMT correction; zero in every file seen in practice
//    12   4  sigfigs        unused
//    16   4  snaplen
//    20   4  network        link type; 1 is Ethernet
//
//   Then, repeated: record header, 16 bytes, followed by `captured` bytes
//     0   4  ts_sec
//     4   4  ts_frac        microseconds or nanoseconds, per the magic
//     8   4  captured       bytes stored in the file
//    12   4  original       bytes the frame had on the wire
//
// The magic is the 32-bit value 0xa1b2c3d4 written in the *writer's* byte order,
// with a variant spelling for nanosecond timestamps. So four byte sequences are
// valid and each one settles two questions at once. IEX HIST files up to
// 2017-06-15 begin `d4 c3 b2 a1`: little-endian, microseconds.
//
// This reader takes the whole file as bytes and never opens anything. File I/O
// belongs to the caller, for two reasons: a library that reads files is a library
// that cannot be driven from a fuzzer or a deterministic simulation, and mapping
// the file is the caller's decision to make. Every test here is therefore an
// in-memory test with no fixture on disk.

#ifndef DFR_CAPTURE_PCAP_HPP
#define DFR_CAPTURE_PCAP_HPP

#include <dfr/capture/frame.hpp>
#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace capture::pcap {

inline constexpr std::size_t kFileHeaderSize = 24;
inline constexpr std::size_t kRecordHeaderSize = 16;

// The four valid magics, as byte sequences rather than as integers, because the
// whole point is that the bytes are what distinguish them.
inline constexpr std::array<std::uint8_t, 4> kMagicBigMicros{0xA1, 0xB2, 0xC3, 0xD4};
inline constexpr std::array<std::uint8_t, 4> kMagicLittleMicros{0xD4, 0xC3, 0xB2, 0xA1};
inline constexpr std::array<std::uint8_t, 4> kMagicBigNanos{0xA1, 0xB2, 0x3C, 0x4D};
inline constexpr std::array<std::uint8_t, 4> kMagicLittleNanos{0x4D, 0x3C, 0xB2, 0xA1};

enum class byte_order : std::uint8_t { little, big };
enum class timestamp_resolution : std::uint8_t { microseconds, nanoseconds };

struct file_info {
  byte_order order{byte_order::little};
  timestamp_resolution resolution{timestamp_resolution::microseconds};
  std::uint16_t version_major{0};
  std::uint16_t version_minor{0};
  std::uint32_t snaplen{0};
  std::uint16_t link{0};

  [[nodiscard]] constexpr bool is_ethernet() const noexcept {
    return link == static_cast<std::uint16_t>(link_type::ethernet);
  }

  [[nodiscard]] friend constexpr bool operator==(const file_info&,
                                                 const file_info&) = default;
};

// Reads records out of a classic pcap file held in memory.
//
// Deliberately not a range or an iterator pair. A record can fail to decode, and
// an iterator has nowhere to put the reason — it would have to either throw or
// silently stop, and "silently stop" on a truncated capture is how a partial file
// gets mistaken for a complete one.
class reader {
 public:
  constexpr reader() noexcept = default;

  [[nodiscard]] static constexpr result<reader> over(packet_view file) noexcept {
    packet_view header;
    if (const auto err = file.prefix(kFileHeaderSize).get(header);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }

    file_info info;
    const std::array<std::uint8_t, 4> magic{header.u8_at(0), header.u8_at(1),
                                            header.u8_at(2), header.u8_at(3)};
    if (magic == kMagicLittleMicros) {
      info.order = byte_order::little;
      info.resolution = timestamp_resolution::microseconds;
    } else if (magic == kMagicBigMicros) {
      info.order = byte_order::big;
      info.resolution = timestamp_resolution::microseconds;
    } else if (magic == kMagicLittleNanos) {
      info.order = byte_order::little;
      info.resolution = timestamp_resolution::nanoseconds;
    } else if (magic == kMagicBigNanos) {
      info.order = byte_order::big;
      info.resolution = timestamp_resolution::nanoseconds;
    } else DFR_UNLIKELY {
      // Includes the pcapng case: a file beginning 0a 0d 0d 0a is a Section
      // Header Block, not a pcap magic. Reporting not_supported rather than a
      // framing error lets a caller try the other reader.
      return error::not_supported;
    }

    const bool little = info.order == byte_order::little;
    info.version_major = read16(header, 4, little);
    info.version_minor = read16(header, 6, little);
    info.snaplen = read32(header, 16, little);
    info.link = static_cast<std::uint16_t>(read32(header, 20, little));

    packet_view records;
    if (const auto err = file.suffix(kFileHeaderSize).get(records);
        err != error::ok) DFR_UNLIKELY {
      return error::truncated_header;
    }
    return reader{info, records};
  }

  [[nodiscard]] constexpr const file_info& info() const noexcept {
    return info_;
  }

  // True once every byte has been consumed by a *complete* record.
  //
  // The invariant that makes this useful: a failed next() consumes nothing. It
  // restores the cursor to where the record began, so a file cut mid-record
  // always leaves done() false and remaining() non-zero.
  //
  // Without that, a file whose last record header is present but whose payload is
  // missing would consume the header, leave nothing behind, and report done() —
  // so a caller could not distinguish a clean end from a capture stopped by a
  // full disk. That distinction is the whole reason to read a capture carefully.
  [[nodiscard]] constexpr bool done() const noexcept { return rest_.empty(); }

  // Bytes left over that do not form a complete record. Zero on a clean file.
  [[nodiscard]] constexpr std::size_t remaining() const noexcept {
    return rest_.size();
  }

  [[nodiscard]] constexpr std::uint64_t records_read() const noexcept {
    return records_read_;
  }

  [[nodiscard]] constexpr result<frame> next() noexcept {
    DFR_ASSERT(!done(), "next() on an exhausted reader; check done() first");

    const bool little = info_.order == byte_order::little;

    // Every failure path below restores this, so a failed read consumes nothing.
    const packet_view at_record_start = rest_;

    packet_view header;
    if (const auto err = rest_.prefix(kRecordHeaderSize).get(header);
        err != error::ok) DFR_UNLIKELY {
      // Fewer than sixteen bytes remain. The file was cut mid-record.
      return error::truncated_header;
    }

    const std::uint32_t seconds = read32(header, 0, little);
    const std::uint32_t fraction = read32(header, 4, little);
    const std::uint32_t captured = read32(header, 8, little);
    const std::uint32_t original = read32(header, 12, little);

    if (const auto err = rest_.consume(kRecordHeaderSize); !err) DFR_UNLIKELY {
      rest_ = at_record_start;
      return err.error_code();
    }

    // A stored length above the wire length is impossible, and it is the shape a
    // byte-order misdetection takes: read the wrong way round, a 74-byte record
    // becomes 1,241,513,984. Refusing it here turns a silent walk off the end
    // into one reported error.
    if (captured > original) DFR_UNLIKELY {
      rest_ = at_record_start;
      return error::message_length_mismatch;
    }

    packet_view data;
    if (const auto err = rest_.prefix(captured).get(data); err != error::ok)
        DFR_UNLIKELY {
      rest_ = at_record_start;
      return error::truncated_block;
    }
    if (const auto err = rest_.consume(captured); !err) DFR_UNLIKELY {
      rest_ = at_record_start;
      return err.error_code();
    }

    ++records_read_;
    return frame{.timestamp_ns = to_nanoseconds(seconds, fraction,
                                                info_.resolution),
                 .data = data,
                 .wire_length = original};
  }

  // Walks to the end, reporting the first failure. The convenience wrapper,
  // built on next() rather than replacing it.
  template <typename Handler>
  [[nodiscard]] constexpr result<void> drain(Handler&& handler) noexcept {
    while (!done()) {
      frame current;
      if (const auto err = next().get(current); err != error::ok) DFR_UNLIKELY {
        return err;
      }
      handler(current);
    }
    return ok();
  }

 private:
  constexpr reader(file_info info, packet_view records) noexcept
      : info_(info), rest_(records) {}

  // Byte order is a runtime property of the file, so the accessor branches. One
  // branch per field on a cold path, rather than a reader type per endianness
  // that would make the caller's type depend on the file it opened.
  [[nodiscard]] static constexpr std::uint16_t read16(packet_view v,
                                                      std::size_t at,
                                                      bool little) noexcept {
    return little ? v.le16_at(at) : v.be16_at(at);
  }
  [[nodiscard]] static constexpr std::uint32_t read32(packet_view v,
                                                      std::size_t at,
                                                      bool little) noexcept {
    return little ? v.le32_at(at) : v.be32_at(at);
  }

  [[nodiscard]] static constexpr std::uint64_t to_nanoseconds(
      std::uint32_t seconds, std::uint32_t fraction,
      timestamp_resolution resolution) noexcept {
    const std::uint64_t scale =
        resolution == timestamp_resolution::microseconds ? 1'000 : 1;
    return static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(fraction) * scale;
  }

  file_info info_{};
  packet_view rest_;
  std::uint64_t records_read_{0};
};

}  // namespace capture::pcap
}  // namespace dfr::inline v1

#endif  // DFR_CAPTURE_PCAP_HPP

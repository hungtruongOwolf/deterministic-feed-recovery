// pcapng block and option constants.
//
// Every block has the same frame:
//
//     0   4  Block Type
//     4   4  Block Total Length   includes these 12 bytes; a multiple of 4
//     8   n  Block Body
//   8+n   4  Block Total Length   repeated, and must equal the first
//
// The repeated length is what makes the format walkable backwards, and it is a
// free integrity check: a reader that ignores the trailing copy accepts a
// corrupted length and then walks to a wrong offset for the rest of the file.

#ifndef DFR_CAPTURE_PCAPNG_CONSTANTS_HPP
#define DFR_CAPTURE_PCAPNG_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>

namespace dfr::inline v1::capture::pcapng {

inline constexpr std::size_t kBlockFrameSize = 12;   // type + length + length
inline constexpr std::size_t kBlockHeaderSize = 8;   // type + leading length

// Block types we act on. Anything else is skipped, which is the point of the
// format: a reader must tolerate blocks it has never heard of.
inline constexpr std::uint32_t kSectionHeaderBlock = 0x0A0D'0D0A;
inline constexpr std::uint32_t kInterfaceDescriptionBlock = 0x0000'0001;
inline constexpr std::uint32_t kSimplePacketBlock = 0x0000'0003;
inline constexpr std::uint32_t kEnhancedPacketBlock = 0x0000'0006;

// The SHB's type is byte-order independent by design: 0x0A0D0D0A reads the same
// from either end, so a reader can recognise a section header before it knows
// the section's byte order. That is the bootstrap the format depends on.
static_assert(((kSectionHeaderBlock >> 24) & 0xFF) ==
                  (kSectionHeaderBlock & 0xFF),
              "the SHB type must be byte-order independent");
static_assert(((kSectionHeaderBlock >> 16) & 0xFF) ==
                  ((kSectionHeaderBlock >> 8) & 0xFF),
              "the SHB type must be byte-order independent");

// Found at body offset 0 of every Section Header Block. Reading it as a native
// 32-bit value is what settles the section's byte order.
inline constexpr std::uint32_t kByteOrderMagic = 0x1A2B'3C4D;

// ---- Section Header Block body ----
inline constexpr std::size_t kShbByteOrderMagicOffset = 0;
inline constexpr std::size_t kShbMajorOffset = 4;
inline constexpr std::size_t kShbMinorOffset = 6;
inline constexpr std::size_t kShbSectionLengthOffset = 8;
inline constexpr std::size_t kShbBodyMinSize = 16;

inline constexpr std::uint16_t kSupportedMajor = 1;

// ---- Interface Description Block body ----
inline constexpr std::size_t kIdbLinkTypeOffset = 0;
inline constexpr std::size_t kIdbSnaplenOffset = 4;
inline constexpr std::size_t kIdbBodyMinSize = 8;

// ---- Enhanced Packet Block body ----
inline constexpr std::size_t kEpbInterfaceOffset = 0;
inline constexpr std::size_t kEpbTimestampHighOffset = 4;
inline constexpr std::size_t kEpbTimestampLowOffset = 8;
inline constexpr std::size_t kEpbCapturedOffset = 12;
inline constexpr std::size_t kEpbOriginalOffset = 16;
inline constexpr std::size_t kEpbBodyMinSize = 20;

// ---- Simple Packet Block body ----
inline constexpr std::size_t kSpbOriginalOffset = 0;
inline constexpr std::size_t kSpbBodyMinSize = 4;

// ---- Options ----
//
// Each option is a 2-byte code, a 2-byte length, then that many bytes padded up
// to a 4-byte boundary. A code of zero with length zero ends the list.
inline constexpr std::size_t kOptionHeaderSize = 4;
inline constexpr std::uint16_t kOptionEndOfOpt = 0;
inline constexpr std::uint16_t kOptionIfTsResolution = 9;

// How many interfaces one section may describe before we give up.
//
// A bound rather than a growable table, per TIGER_STYLE's "put a limit on
// everything" and "no memory may be dynamically allocated after
// initialization". A merged capture of every IEX channel is far below this.
inline constexpr std::size_t kMaxInterfaces = 64;

// Round a length up to the 4-byte boundary the format pads to.
//
// Packet data is padded and the padding is not data. A reader that walks by the
// unpadded length lands one to three bytes early on the next option and decodes
// garbage.
[[nodiscard]] constexpr std::size_t pad4(std::size_t length) noexcept {
  return (length + 3) & ~std::size_t{3};
}

static_assert(pad4(0) == 0);
static_assert(pad4(1) == 4);
static_assert(pad4(4) == 4);
static_assert(pad4(5) == 8);

}  // namespace dfr::inline v1::capture::pcapng
#endif  // DFR_CAPTURE_PCAPNG_CONSTANTS_HPP

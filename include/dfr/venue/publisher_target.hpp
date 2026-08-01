// What a publisher needs to know about a wire protocol.
//
// This abstraction was deliberately not written when the IEX-TP publisher landed. The comment then said
// so: *"A MoldUDP64 publisher is the same shape with a different encoder and no stream offset. The
// injector is parameterised over a protocol because two implementations existed to factor; here there is
// one, and building the policy first would be designing for a case not yet written."*
//
// The second case has now been written, so the policy exists and it is shaped by what actually differed
// rather than by what might have.
//
// What actually differed
// --------------------
// Three things, and only three. The builder type. The heartbeat encoder's arguments. And whether the
// protocol maintains a **stream offset**: a byte position alongside the message sequence, which IEX-TP
// has and MoldUDP64 does not. That third one is not cosmetic: it is IEX-TP's second redundant chain, the
// thing that catches a corrupted length field which sequence numbers alone cannot see. A shared
// publisher that maintained it for both would write a field MoldUDP64 has no room for.
//
// Everything else: packing until the datagram is full, never splitting a message, advancing the
// sequence by the message count, emitting a heartbeat after a quiet period: is protocol-independent,
// which is why one publisher can now drive both.

#ifndef DFR_VENUE_PUBLISHER_TARGET_HPP
#define DFR_VENUE_PUBLISHER_TARGET_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/wire/iextp/encode.hpp>
#include <dfr/wire/moldudp64/encode.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1::venue {

// Where a publisher is in the stream. Passed whole rather than as four arguments so a target cannot pick
// the wrong two, and so the one field a protocol may ignore is visibly ignored.
struct stream_position {
  std::uint32_t session{0};
  std::uint32_t channel{0};
  std::uint64_t next_sequence{0};
  // Bytes of payload published so far. Meaningful only where the protocol carries it.
  std::int64_t next_stream_offset{0};
};

template <typename T>
concept publisher_target = requires(mutable_packet_view buffer,
                                    const stream_position& where) {
  typename T::builder;
  { T::kName } -> std::convertible_to<std::string_view>;
  { T::kHeaderSize } -> std::convertible_to<std::size_t>;
  // True where the protocol carries a byte offset beside the message sequence.
  { T::kTracksStreamOffset } -> std::convertible_to<bool>;
  { T::open(buffer, where) } -> std::same_as<result<typename T::builder>>;
  { T::heartbeat(buffer, where) } -> std::same_as<result<std::size_t>>;
};

// ---------------------------------------------------------------------------
// IEX-TP
// ---------------------------------------------------------------------------

struct iextp_target {
  using builder = wire::iextp::packet_builder;

  static constexpr std::string_view kName = "IEX-TP";
  static constexpr std::size_t kHeaderSize = wire::iextp::kHeaderSize;
  static constexpr bool kTracksStreamOffset = true;

  [[nodiscard]] static constexpr result<builder> open(
      mutable_packet_view buffer, const stream_position& where) noexcept {
    const wire::iextp::header prototype{
        .channel = where.channel,
        .session = where.session,
        .stream_offset = where.next_stream_offset,
        .first_sequence = where.next_sequence};
    return builder::into(buffer, prototype);
  }

  [[nodiscard]] static constexpr result<std::size_t> heartbeat(
      mutable_packet_view buffer, const stream_position& where) noexcept {
    const wire::iextp::header prototype{
        .channel = where.channel,
        .session = where.session,
        .stream_offset = where.next_stream_offset,
        .first_sequence = where.next_sequence};
    return wire::iextp::encode_heartbeat(buffer, prototype);
  }
};

// ---------------------------------------------------------------------------
// MoldUDP64
// ---------------------------------------------------------------------------

struct moldudp64_target {
  using builder = wire::moldudp64::packet_builder;

  static constexpr std::string_view kName = "MoldUDP64";
  static constexpr std::size_t kHeaderSize = wire::moldudp64::kHeaderSize;
  // No stream offset. A receiver on this protocol has sequence numbers and nothing else to check them
  // against, which is why dfr::wire::iextp's three-chain check has no MoldUDP64 counterpart.
  static constexpr bool kTracksStreamOffset = false;

  [[nodiscard]] static constexpr result<builder> open(
      mutable_packet_view buffer, const stream_position& where) noexcept {
    return builder::into(buffer, session_for(where.session), where.next_sequence);
  }

  [[nodiscard]] static constexpr result<std::size_t> heartbeat(
      mutable_packet_view buffer, const stream_position& where) noexcept {
    return wire::moldudp64::encode_heartbeat(buffer, session_for(where.session),
                                             where.next_sequence);
  }

  [[nodiscard]] static constexpr result<std::size_t> end_of_session(
      mutable_packet_view buffer, const stream_position& where) noexcept {
    return wire::moldudp64::encode_end_of_session(buffer, session_for(where.session),
                                                  where.next_sequence);
  }

 private:
  // MoldUDP64's session is ten bytes of text, not an integer. The numeric session a publisher is
  // configured with is written into the last four so the field stays printable and two configured
  // sessions stay distinguishable: the same convention chaos::moldudp64_target uses when it rewrites
  // the field, so a fault injected into a published stream lands where a reader expects it.
  [[nodiscard]] static constexpr wire::moldudp64::session_id session_for(
      std::uint32_t session) noexcept {
    std::array<char, wire::moldudp64::kSessionSize> text{};
    text.fill(' ');
    std::uint32_t rest = session;
    for (std::size_t i = 0; i < 4; ++i) {
      text[wire::moldudp64::kSessionSize - 1 - i] =
          static_cast<char>('0' + rest % 10);
      rest /= 10;
    }
    wire::moldudp64::session_id id;
    // Asserted rather than defaulted: the text was just built here, exactly ten printable bytes, so a
    // failure would be a bug in the line above. value_or() would have published the wrong session and
    // said nothing.
    const auto err = wire::moldudp64::session_id::from_text(
                         std::string_view{text.data(), text.size()})
                         .get(id);
    DFR_ASSERT(err == error::ok, "a generated session id must be well formed");
    return id;
  }
};

static_assert(publisher_target<iextp_target>);
static_assert(publisher_target<moldudp64_target>);

// The two protocols differ in header size, which is pinned here so a target that used the wrong constant
// would fail to compile rather than produce packets a reader silently misframes.
static_assert(iextp_target::kHeaderSize == 40);
static_assert(moldudp64_target::kHeaderSize == 20);
static_assert(iextp_target::kTracksStreamOffset);
static_assert(!moldudp64_target::kTracksStreamOffset);

}  // namespace dfr::inline v1::venue
#endif  // DFR_VENUE_PUBLISHER_TARGET_HPP

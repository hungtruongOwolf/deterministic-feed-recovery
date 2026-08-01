// What crosses the thread boundary: one delivered message, flat.
//
// Split from spsc_ring.hpp because the ring does not care what it carries and a reader checking the record's
// shape does not need the memory ordering argument. The same seam order_outcome.hpp and client_state.hpp sit
// on.
//
// Why a fixed-size body rather than a pointer
// ------------------------------------------
// A ring of pointers would be smaller and would be wrong here. The producer would have to keep the pointed-to
// bytes alive until a consumer it cannot see is finished with them, which means either a second ring going
// back or a lifetime rule enforced by comment. And the consumer would pay a dependent load: the pointer, then
// the cache miss it leads to: on the critical path, which is the cost this whole structure exists to avoid.
//
// So the body is copied into the slot. That is a memcpy of a bounded size, and it buys an ownership story with
// no rules in it: once `push` returns true the producer is done, forever.

#ifndef DFR_CONCURRENT_DELIVERY_HPP
#define DFR_CONCURRENT_DELIVERY_HPP

#include <dfr/core/packet_view.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace concurrent {

// The largest message body that crosses the boundary.
//
// 256 bytes because every message in the protocols this library speaks fits: the widest OUCH acknowledgement
// is under 100 and IEX-TP message bodies are smaller still. A message that does not fit is refused at the
// boundary rather than truncated, because a truncated message decodes into a *plausible* wrong one.
inline constexpr std::size_t kMaxDeliveryBytes = 256;

// Why the sequence number travels with the body
// ---------------------------------------------
// The consumer is on another thread and cannot ask the client anything: by the time it looks, the client has
// moved on. So every field it needs to make sense of the message is in the record. This is the same rule the
// trace format follows(carry the conclusions, do not make the reader recompute them) applied to a thread
// boundary instead of a file.
struct delivery {
  std::uint64_t sequence{0};

  // Which line it arrived on, or zero for a message that came back from a retransmit or a snapshot. A
  // consumer measuring line health needs this and cannot derive it.
  std::uint8_t line{0};

  // True when this message was recovered rather than received live. A consumer that treats a repair
  // differently(most risk systems do) cannot tell otherwise.
  bool recovered{false};

  std::uint16_t size{0};
  std::array<std::byte, kMaxDeliveryBytes> body{};

  [[nodiscard]] constexpr packet_view payload() const noexcept {
    return packet_view{body.data(), size};
  }

  // Builds a record, or refuses. Returns false when the body does not fit, which is a caller error the caller
  // must see: silently truncating would hand the consumer a message that decodes into a different one.
  [[nodiscard]] static constexpr bool from(std::uint64_t sequence, std::uint8_t line, bool recovered,
                                           packet_view message, delivery& into) noexcept {
    if (message.size() > kMaxDeliveryBytes) {
      return false;
    }
    into.sequence = sequence;
    into.line = line;
    into.recovered = recovered;
    into.size = static_cast<std::uint16_t>(message.size());
    for (std::size_t i = 0; i < message.size(); ++i) {
      into.body[i] = static_cast<std::byte>(message.u8_at(i));
    }
    return true;
  }
};

}  // namespace concurrent
}  // namespace dfr::inline v1

#endif  // DFR_CONCURRENT_DELIVERY_HPP

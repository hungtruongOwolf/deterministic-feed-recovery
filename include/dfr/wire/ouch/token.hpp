// An OUCH order token: fourteen bytes the client chooses, and the only identity an order has.
//
// From OUCH 4.2 §1.2: tokens are alphanumeric, letters, numbers and spaces are all allowed, they are
// **case sensitive**, and they must be **day-unique per OUCH account**.
//
// Why this is a type and not a string
// ----------------------------------
// The token is what makes the protocol's failover model work. §2: "All Inbound Messages may be
// repeated benignly. This gives the client the ability to re-send any Inbound message if it is
// uncertain whether NASDAQ received it in the case of a connection loss or an application error."
// Re-sending is safe *only* because a duplicate token is recognised and ignored, so the token is the
// idempotency key for order entry, and comparing two of them wrongly either loses an order or
// duplicates one.
//
// The comparison has to be exact over all fourteen bytes, including trailing spaces, because the
// field is fixed-width and space-padded: "ABC" and "ABC " are the same token on the wire and must
// compare equal, while case must not be folded.

#ifndef DFR_WIRE_OUCH_TOKEN_HPP
#define DFR_WIRE_OUCH_TOKEN_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dfr::inline v1::wire::ouch {

inline constexpr std::size_t kTokenSize = 14;

class order_token {
 public:
  // Space-filled, which is the wire's idea of empty. A zero-filled token would be fourteen NUL bytes,
  // which is a *different* token rather than an absent one.
  constexpr order_token() noexcept { bytes_.fill(std::byte{' '}); }

  // Builds from text, padding right with spaces. Refuses text that does not fit rather than
  // truncating: two orders whose tokens differ only past the fourteenth character would silently
  // become one order, and the second would be ignored as a duplicate.
  [[nodiscard]] static constexpr result<order_token> from_text(
      std::string_view text) noexcept {
    if (text.size() > kTokenSize) DFR_UNLIKELY {
      return error::invalid_argument;
    }
    order_token out;
    for (std::size_t i = 0; i < text.size(); ++i) {
      out.bytes_[i] = static_cast<std::byte>(text[i]);
    }
    return out;
  }

  [[nodiscard]] static constexpr result<order_token> from_bytes(
      packet_view field) noexcept {
    if (field.size() != kTokenSize) DFR_UNLIKELY {
      return error::message_length_mismatch;
    }
    order_token out;
    for (std::size_t i = 0; i < kTokenSize; ++i) {
      out.bytes_[i] = static_cast<std::byte>(field.u8_at(i));
    }
    return out;
  }

  constexpr void write_to(mutable_packet_view field) const noexcept {
    DFR_ASSERT(field.contains(0, kTokenSize), "a token needs fourteen bytes");
    for (std::size_t i = 0; i < kTokenSize; ++i) {
      field.put_u8_at(i, static_cast<std::uint8_t>(bytes_[i]));
    }
  }

  // The whole fourteen bytes, padding included, for writing and for hashing.
  [[nodiscard]] constexpr packet_view bytes() const noexcept DFR_LIFETIME_BOUND {
    return packet_view{bytes_.data(), bytes_.size()};
  }

  // The content without its trailing padding, for a report. Not for comparison: comparison is over
  // all fourteen bytes, which is both cheaper and exactly what the wire means.
  // Not `constexpr`, and GCC is the reason it says so out loud.
//
  // A `std::string_view` over `std::byte` needs a `reinterpret_cast`, and a reinterpret_cast is **forbidden in
  // constant evaluation**. Clang accepted the `constexpr` because nothing ever constant-evaluated it, so the
  // invalid path was never instantiated; GCC 14 diagnoses it eagerly and is right. The keyword was a claim the
  // function could not honour, and anybody who took it up would have got a hard error rather than a slow function.
//
  // Dropping it is the truthful fix. Keeping it would need the bytes to be `char` underneath, which would mean
  // `packet_view` giving up `std::byte`, and `std::byte` is what stops a byte being arithmetic by accident.
  [[nodiscard]] std::string_view text() const noexcept DFR_LIFETIME_BOUND {
    std::size_t length = kTokenSize;
    while (length > 0 && bytes_[length - 1] == std::byte{' '}) {
      --length;
    }
    return std::string_view{reinterpret_cast<const char*>(bytes_.data()), length};
  }

  // Not constexpr either, because `text()` cannot be: see above.
  [[nodiscard]] bool empty() const noexcept { return text().empty(); }

  // Exact over all fourteen bytes: case sensitive, padding significant only in that it is part of the
  // field. `from_text("ABC")` and a wire token of "ABC" plus eleven spaces are the same token.
  [[nodiscard]] friend constexpr bool operator==(const order_token& a,
                                                 const order_token& b) noexcept {
    return a.bytes_ == b.bytes_;
  }

  // Ordered so a fixed-capacity registry can keep tokens sorted and find one by binary search without
  // hashing or allocating.
  [[nodiscard]] friend constexpr bool operator<(const order_token& a,
                                                const order_token& b) noexcept {
    return a.bytes_ < b.bytes_;
  }

 private:
  std::array<std::byte, kTokenSize> bytes_{};
};

}  // namespace dfr::inline v1::wire::ouch
#endif  // DFR_WIRE_OUCH_TOKEN_HPP

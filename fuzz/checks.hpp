// What each decoder is asked, when the bytes are hostile.
//
// Every decoder in this library takes bytes off a network, and a network hands you whatever it likes. The
// existing tests feed them bytes chosen by a person, which finds the cases a person thought of — and the whole
// premise of the project is that the interesting failures are the ones nobody thought of.
//
// One header rather than one file per protocol, because the *checks* are the reviewable part and having them
// side by side is what makes it possible to see that they are the same three checks everywhere. Each target
// file is then ten lines of plumbing.
//
// The three checks, and why "it did not crash" is the weakest of them
// -----------------------------------------------------------------
//   1. **No crash, no undefined behaviour.** Address and undefined sanitizers are on, so a read past the end
//      or a signed overflow fails here rather than becoming a subtle wrong answer in production. This is the
//      check a fuzzer is usually bought for and it is the least interesting one.
//   2. **A success has to be self-consistent.** If a decoder says it read a packet of N bytes, N must be
//      within the input. If it hands back a view, that view must lie inside the input. A decoder returning a
//      length it did not have is how a caller ends up reading somebody else's memory *legitimately*, with the
//      sanitizer silent because the pointer arithmetic was correct and the length was a lie.
//   3. **Framing must be total.** Walking a stream must either consume it or stop; it must never loop without
//      advancing. An unbounded loop on hostile input is a denial of service that no memory checker reports.
//
// Nothing here asserts what a decode *means*. The unit tests do that against real capture bytes. A fuzzer that
// asserted semantics would mostly assert the fuzzer's own idea of the protocol.

#ifndef DFR_FUZZ_CHECKS_HPP
#define DFR_FUZZ_CHECKS_HPP

#include <dfr/capture/ethernet.hpp>
#include <dfr/capture/pcap.hpp>
#include <dfr/capture/pcapng.hpp>
#include <dfr/wire/deep.hpp>
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/cursor.hpp>
#include <dfr/wire/iextp/header.hpp>
#include <dfr/wire/moldudp64/cursor.hpp>
#include <dfr/wire/moldudp64/header.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace dfr_fuzz {

// A failed invariant aborts. Not an exception and not a return code: the point is to stop at the input that
// broke it, with the stack intact, and a fuzzer's only contract is the process exit.
inline void require(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "fuzz invariant broken: %s\n", what);
    std::abort();
  }
}

// Check 2, applied to a view: whatever a decoder handed back must lie inside what it was given.
inline void inside(dfr::packet_view whole, dfr::packet_view part, const char* what) {
  if (part.empty()) {
    return;
  }
  require(part.data() >= whole.data(), what);
  require(part.data() + part.size() <= whole.data() + whole.size(), what);
}

// ---------------------------------------------------------------------------
// IEX-TP
// ---------------------------------------------------------------------------

inline void fuzz_iextp(dfr::packet_view input) {
  namespace iex = dfr::wire::iextp;

  iex::header header;
  const auto decoded = iex::decode_header(input);
  if (decoded.get(header) == dfr::error::ok) {
    // The framing verifier is the interesting one: it is what a receiver trusts before believing a count.
    (void)iex::verify_payload_framing(input);
  }

  auto cursor = iex::message_cursor::over(input);
  if (!cursor) {
    return;
  }
  auto walk = cursor.value();
  // Check 3. The bound is generous and finite: a stream of N bytes cannot legitimately yield more than N
  // messages, because every message costs at least its length prefix.
  std::size_t steps = 0;
  const std::size_t limit = input.size() + 2;
  while (!walk.done()) {
    const auto next = walk.next();
    if (!next) {
      break;
    }
    inside(input, next.value().payload, "an IEX-TP message pointed outside its packet");
    require(++steps <= limit, "walking an IEX-TP packet did not terminate");
  }
}

// ---------------------------------------------------------------------------
// MoldUDP64
// ---------------------------------------------------------------------------

inline void fuzz_moldudp64(dfr::packet_view input) {
  namespace mold = dfr::wire::moldudp64;

  mold::header header;
  (void)mold::decode_header(input).get(header);

  auto cursor = mold::message_cursor::over(input);
  if (!cursor) {
    return;
  }
  auto walk = cursor.value();
  std::size_t steps = 0;
  const std::size_t limit = input.size() + 2;
  while (!walk.done()) {
    const auto next = walk.next();
    if (!next) {
      break;
    }
    inside(input, next.value().payload, "a MoldUDP64 message pointed outside its packet");
    require(++steps <= limit, "walking a MoldUDP64 packet did not terminate");
  }
}

// ---------------------------------------------------------------------------
// SoupBinTCP — a stream, so the interesting property is that framing makes progress
// ---------------------------------------------------------------------------

inline void fuzz_soupbintcp(dfr::packet_view input) {
  namespace soup = dfr::wire::soupbintcp;

  soup::stream_cursor cursor{1};
  std::size_t at = 0;
  std::size_t steps = 0;
  while (at < input.size()) {
    const auto rest = input.subview(at, input.size() - at);
    if (!rest) {
      break;
    }
    soup::sequenced_packet next;
    if (cursor.next(rest.value()).get(next) != dfr::error::ok) {
      break;  // need_more_bytes, or a frame this stream cannot resynchronise on
    }
    // The property a stream framer must have and can silently lack: it consumed something. A zero-length frame
    // would loop forever on a connection that is otherwise fine.
    require(next.frame.frame_size > 0, "a SoupBinTCP frame consumed no bytes");
    require(at + next.frame.frame_size <= input.size(),
            "a SoupBinTCP frame claimed more bytes than the stream held");
    inside(input, next.frame.payload, "a SoupBinTCP payload pointed outside its stream");
    at += next.frame.frame_size;
    require(++steps <= input.size() + 2, "framing a SoupBinTCP stream did not terminate");

    // The login bodies are the ones with ASCII arithmetic in them, and a 20-digit sequence field is where an
    // overflow is reachable.
    if (next.frame.type == soup::packet_type::login_accepted) {
      soup::login_accepted accepted;
      (void)soup::decode_login_accepted(next.frame.payload).get(accepted);
    } else if (next.frame.type == soup::packet_type::login_request) {
      soup::login_request request;
      (void)soup::decode_login_request(next.frame.payload).get(request);
    }
  }
}

// ---------------------------------------------------------------------------
// OUCH — every inbound and outbound decoder, on the same bytes
// ---------------------------------------------------------------------------

inline void fuzz_ouch(dfr::packet_view input) {
  namespace ouch = dfr::wire::ouch;

  // All of them against the same input on purpose. A message handed to the wrong decoder is exactly what a
  // caller dispatching on a type byte does when the byte is hostile, and each must refuse rather than read.
  ouch::enter_order enter;
  (void)ouch::decode_enter_order(input).get(enter);
  ouch::replace_order replace;
  (void)ouch::decode_replace_order(input).get(replace);
  ouch::cancel_order cancel;
  (void)ouch::decode_cancel_order(input).get(cancel);
  ouch::modify_order modify;
  (void)ouch::decode_modify_order(input).get(modify);

  ouch::accepted accepted;
  (void)ouch::decode_accepted(input).get(accepted);
  ouch::executed executed;
  (void)ouch::decode_executed(input).get(executed);
  ouch::canceled canceled;
  (void)ouch::decode_canceled(input).get(canceled);
}

// ---------------------------------------------------------------------------
// DEEP — the newest decoders, and therefore the least exercised
// ---------------------------------------------------------------------------

inline void fuzz_deep(dfr::packet_view input) {
  namespace deep = dfr::wire::deep;

  deep::header head;
  if (deep::decode_header(input).get(head) != dfr::error::ok) {
    return;
  }
  // The header succeeded, so the length matches the type and every decoder below is entitled to its fields.
  // Anything that goes wrong past this point is a layout error rather than a truncation.
  require(input.size() == deep::expected_size(head.type),
          "a DEEP header succeeded on a length that does not match its type");

  deep::price_level_update update;
  if (deep::decode_price_level(input).get(update) == dfr::error::ok) {
    require(update.symbol.size() <= deep::kSymbolSize, "a DEEP symbol was longer than its field");
  }
  deep::trade_report trade;
  (void)deep::decode_trade(input).get(trade);
  deep::trading_status status;
  (void)deep::decode_trading_status(input).get(status);
  deep::security_directory directory;
  (void)deep::decode_directory(input).get(directory);
  deep::system_event event;
  (void)deep::decode_system_event(input).get(event);
  deep::other_message other;
  (void)deep::decode_other(input).get(other);
}

// ---------------------------------------------------------------------------
// Capture files — the largest attack surface, because a pcap is a file somebody sent you
// ---------------------------------------------------------------------------

inline void fuzz_capture(dfr::packet_view input) {
  namespace cap = dfr::capture;

  if (auto pcap = cap::pcap::reader::over(input); pcap) {
    auto reader = pcap.value();
    std::size_t steps = 0;
    while (!reader.done()) {
      const auto record = reader.next();
      if (!record) {
        break;
      }
      inside(input, record.value().data, "a pcap record pointed outside the file");
      (void)cap::parse_ethernet_udp(record.value().data);
      require(++steps <= input.size() + 2, "reading a pcap did not terminate");
    }
  }

  if (auto ng = cap::pcapng::reader::over(input); ng) {
    auto reader = ng.value();
    std::size_t steps = 0;
    while (!reader.done()) {
      const auto record = reader.next();
      if (!record) {
        break;
      }
      inside(input, record.value().data, "a pcapng record pointed outside the file");
      (void)cap::parse_ethernet_udp(record.value().data);
      require(++steps <= input.size() + 2, "reading a pcapng did not terminate");
    }
  }
}

}  // namespace dfr_fuzz

#endif  // DFR_FUZZ_CHECKS_HPP

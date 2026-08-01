// MoldUDP64: NASDAQ's sequenced multicast transport.
//
// Layout, from the Nasdaq MoldUDP64 Protocol Specification V1.00:
//
//   offset  size  field
//        0    10  Session          alphanumeric, left-justified, space-padded
//       10     8  Sequence Number  big-endian, of the FIRST message in the packet
//       18     2  Message Count    big-endian
//       20     -  Message Blocks   Message Count of them, each:
//                                    2 bytes big-endian Message Length
//                                    Message Length bytes of payload
//
// The single most important semantic, and the one implementations get wrong:
//
//   **Sequence Number counts MESSAGES, not PACKETS.** A packet carrying three
//   messages at sequence 100 means messages 100, 101 and 102, and the next
//   packet must begin at 103. A client that increments by one per packet falls
//   behind by (message_count - 1) on every packet and then reports a gap that
//   does not exist, or worse, silently accepts one that does.
//
// Two special packets, both signalled through Message Count:
//
//   Message Count 0       heartbeat. Carries no messages, and its Sequence
//                         Number is the *next* sequence the publisher will
//                         send. So a heartbeat advances a watermark; treating
//                         it as a data packet at that sequence reports a
//                         spurious gap. (astra-feed-engine gets this right and
//                         most recent repositories do not.)
//   Message Count 0xFFFF  end of session. No more data will follow.
//
// This header decodes and encodes. Encoding is not an afterthought: dfr::venue
// has to produce byte-identical packets in order to test a client, and
// dfr::chaos has to rewrite sequence numbers in place. The survey found that
// ITCH decoders are saturated while encoders that behave like an exchange
// number roughly zero, so the encode side is a first-class part of the library.
#ifndef DFR_WIRE_MOLDUDP64_HPP
#define DFR_WIRE_MOLDUDP64_HPP

// Umbrella. Include a part directly when that is all you need; see
// docs/STYLE.md section 1.10.
#include <dfr/wire/moldudp64/constants.hpp>
#include <dfr/wire/moldudp64/cursor.hpp>
#include <dfr/wire/moldudp64/encode.hpp>
#include <dfr/wire/moldudp64/header.hpp>
#include <dfr/wire/moldudp64/session_id.hpp>

#endif  // DFR_WIRE_MOLDUDP64_HPP

// IEX-TP: IEX's sequenced multicast transport.
//
// Layout, from the IEX Transport Specification. Every field is LITTLE-endian,
// which is the opposite of MoldUDP64 and the reason byte_order.hpp puts the
// order in each accessor's name rather than inferring it:
//
//   offset  size  field
//        0     1  Version                        0x01
//        1     1  Reserved
//        2     2  Message Protocol ID            identifies DEEP, TOPS, ...
//        4     4  Channel ID
//        8     4  Session ID
//       12     2  Payload Length                 bytes of message blocks
//       14     2  Message Count
//       16     8  Stream Offset                  signed byte offset of the first
//                                                message within the session
//       24     8  First Message Sequence Number
//       32     8  Send Time                      nanoseconds since Unix epoch
//       40     -  Message Blocks                 Message Count of them, each:
//                                                  2 bytes LE Message Length
//                                                  Message Length bytes
//
// What IEX-TP has that MoldUDP64 does not, and why it matters here:
//
//   Payload Length and Stream Offset are *redundant* with the block framing and
//   with the sequence numbers respectively. That redundancy is a gift to a
//   correctness oracle. A receiver can verify three independent chains across
//   consecutive packets:
//
//     first_sequence + message_count  ==  next packet's first_sequence
//     stream_offset   + payload_length ==  next packet's stream_offset
//     sum of block lengths + 2 per block == payload_length
//
//   If any two disagree, something is wrong that a single chain could not have
//   detected. MoldUDP64 offers only the first of the three, so a corrupted
//   length field there is invisible until the book goes wrong. This is why the
//   free IEX HIST corpus is the right thing to build against first.
//
// ---------------------------------------------------------------------------
// VERIFIED against real captures, 2026-07-30
// ---------------------------------------------------------------------------
//
// The field offsets above were transcribed from a specification whose live URL
// now serves a one-page "this document has moved" stub, so they had a single
// source and this header previously carried a note saying so. They have now been
// checked against real IEX HIST data with `tools/inspect`:
//
//   20170826 DEEP, classic pcap, complete file, 3,405,890 bytes
//     20,145 frames, all decoded as IEX-TP, 48,635 messages,
//     13,220 heartbeats, VLAN 1013, group 233.215.21.4:10378,
//     read to the end of the file cleanly, ZERO chain breaks.
//
//   20191224 DEEP, pcapng, first 12 MB of the gzip stream (52,297,728 bytes
//   decompressed)
//     348,103 frames, all decoded as IEX-TP, 380,611 messages,
//     4,042 heartbeats, NO VLAN tag, group 233.215.21.4:10378,
//     ZERO chain breaks. The read stops at the artificial end of the prefix,
//     which the reader reports rather than mistaking for a clean end.
//
// Zero chain breaks across 368,248 real packets means all three chains held on
// every one of them: sequence numbers chained, stream offsets chained, and the
// block framing accounted for exactly the declared payload length. Three
// redundant checks agreeing that many times is strong evidence the offsets are
// right: a wrong Stream Offset or Payload Length offset would have produced a
// break on the second packet.
//
// Two facts the exercise turned up that are worth keeping:
//
//   * **The VLAN tag is not consistent across the corpus.** The 2017 file carries
//     VLAN 1013; the 2019 one carries no tag at all. A reader tested against only
//     one of them would break on the other, and would report the failure as "this
//     file contains no IP traffic".
//
//   * **The format switch is not a clean date boundary.** Trading days from at
//     least 2017-07-03 are pcapng, but the 2017-08-26 Saturday file is classic
//     pcap. So a tool cannot pick a reader by date; it has to try one and fall
//     back, which is why pcap::reader::over returns not_supported rather than a
//     framing error on a pcapng magic.

#ifndef DFR_WIRE_IEXTP_HPP
#define DFR_WIRE_IEXTP_HPP

// Umbrella. Include a part directly when that is all you need; see
// docs/STYLE.md section 1.10.
#include <dfr/wire/iextp/chain.hpp>
#include <dfr/wire/iextp/constants.hpp>
#include <dfr/wire/iextp/cursor.hpp>
#include <dfr/wire/iextp/encode.hpp>
#include <dfr/wire/iextp/header.hpp>

#endif  // DFR_WIRE_IEXTP_HPP

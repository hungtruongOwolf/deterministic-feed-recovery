// IEX-TP — IEX's sequenced multicast transport.
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
// LIMIT, stated deliberately rather than discovered later
// ---------------------------------------------------------------------------
//
// The field offsets above are transcribed from the specification. They have NOT
// yet been validated against a real capture. The live URL for IEX-TP 1.25 now
// serves a one-page "this document has moved" stub, and the complete 15-page
// version is only in the Internet Archive, so the transcription has a single
// source.
//
// Until a real IEX HIST pcap has been parsed end to end with all three chains
// above holding across every packet, treat this decoder as unverified. The
// tests here are self-consistency tests: they prove the encoder and decoder
// agree with each other and with hand-assembled bytes matching the table above.
// They cannot prove the table is right.
//
// Validating against `iextrading.com/api/1.0/hist` is the next task, and the
// check that settles it is that a whole day of DEEP decodes with zero chain
// breaks — which is exactly the measurement that says the layout is correct.

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

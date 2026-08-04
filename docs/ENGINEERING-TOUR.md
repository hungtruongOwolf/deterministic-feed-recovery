# Three-minute engineering tour

One packet, from the venue to the book. Each boundary below names the fact it owns, the check that
judges it, and a command that reaches it. This is the shortest path through the repository for a
reader deciding whether the components form one system or only sit beside one another.

## 1. The venue produces a real stream shape

`venue::publisher<Clock, Target>` packs several messages into a datagram, advances the message
sequence, maintains IEX-TP's stream-offset chain when that protocol carries one, and sends
heartbeats without advancing either position.

It is parameterised by the transport policy rather than copied for IEX-TP and MoldUDP64:

```cpp
using iextp_publisher = publisher<Clock, iextp_target, MaxDatagram>;
using moldudp64_publisher = publisher<Clock, moldudp64_target, MaxDatagram>;
```

The receiver is therefore tested against venue behaviour: packet packing, quiet periods and
cross-packet chains, not a fixture that emits one convenient message per packet.

```sh
./build/dev/tests/dfr_venue_tests "a published stream satisfies both chains"
```

## 2. Chaos damages only the wire

`chaos::injector<Target>` applies a seeded schedule. One offered packet can produce zero, one or two
emissions; delayed packets live in a fixed queue and mutated bytes live in a fixed scratch buffer.
The same `(seed, packet_index)` produces the same damage.

The injector reports what actually happened, including faults a short packet could not carry.
That distinction lets the oracle compare received damage with applied damage rather than with the
schedule's intention.

```sh
./build/dev/tests/dfr_chaos_tests "the same schedule injects identically twice"
```

## 3. Wire decoding refuses malformed claims

The transport decoder validates the header and framing before recovery sees sequence fields. A
message cursor must either consume bytes or stop; hostile lengths cannot make it walk outside the
datagram or loop without advancing.

Six packet decoders are fuzzed from real capture bytes or bytes emitted by the project's encoders.
The seventh target interprets bytes as legal calls into the recovery state machine.

```sh
./build/fuzz/fuzz/fuzz_iextp --seed 1 --rounds 250000 fuzz/corpus/iextp/*
```

## 4. Recovery components own separate facts

`recovery::client` composes four components:

| component | the one fact it owns |
|---|---|
| `arbiter` | which part of redundant A/B input is new |
| `gap_tracker` | which sequence ranges are still missing |
| `requester` | when to request, retry or abandon each range |
| `replay_buffer` | live messages held while a snapshot is built |

No component opens a socket or reads a clock. `poll(now)` returns an action and the caller performs
the I/O. If retransmission has expired, the client asks for a snapshot rather than allowing a known
hole to disappear from its accounting.

```sh
./build/dev/tests/dfr_integration_tests "a client recovers through a real retransmit facility"
```

## 5. Sequence order is restored before another core sees data

Recovery deliberately accepts live messages beyond an open hole. When the missing message returns,
arrival order is therefore `1, 2, 4, 5, 3`. Applying that directly to a last-write-wins book can
leave an older value on top of a newer one.

`concurrent::publisher` holds a fixed window before its SPSC ring. Only the sequence-ordered stream
crosses the thread boundary, so the consumer is a memoryless `pop, decode, apply` loop. A full ring
or reorder window is refused and counted, never overwritten.

```sh
./build/dev/tests/dfr_integration_tests "the publisher orders before the boundary*"
```

## 6. The oracle asks about content, not bookkeeping

One DEEP stream is consumed twice. The reference path receives every message in order. The subject
path passes the same packets through fault injection, recovery and ordered delivery. Both paths use
the same decoder and book, so their only difference is whether recovery sat between feed and book.

The load-bearing assertion is:

```cpp
CHECK(through.books == reference.books);
```

The negative control applies repairs in arrival order. It delivers the same number of messages and
builds a different book, proving that a sequence count alone is not the invariant.

A second oracle keeps individual Nasdaq ITCH order identity rather than only aggregate price
levels. It drives 560 Add, Execute, Cancel, Replace and Delete messages through MoldUDP64 loss,
delay, duplication, retransmission and ordered delivery, then compares the complete live-order set
with a loss-free replay.

```sh
./build/dev/tests/dfr_integration_tests "applying a gap-filling feed in arrival order*"
./build/dev/tests/dfr_integration_tests "[integration][order-level]"
```

## 7. The same decisions become an inspectable trace

`trace::recorder` observes the run; it does not participate. Every JSONL event carries the resulting
client state and book figures, so the viewer draws one source of truth instead of implementing a
second recovery state machine in TypeScript.

The browser calls the same C++ through WebAssembly. CI compares six native and browser runs byte for
byte.

```sh
./build/dev/tools/trace --seed 4711 --messages 300 --faults 6 --out /tmp/run.jsonl
./scripts/check-wasm.sh dev
```

## The complete real-data check

The committed IEX capture contains 20,145 packets and 48,635 messages. This command injects faults,
decodes what survives, serves retransmits from the undamaged input, and checks exact detection,
exactly-once delivery and balanced accounting:

```sh
gunzip -c captures/20170826-iex-deep.pcap.gz >/tmp/deep.pcap
./build/dev/tools/verify /tmp/deep.pcap --seed 4711 --faults 40
```

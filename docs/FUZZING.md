# Fuzzing

Every decoder in this library takes bytes off a network, and a network hands you whatever it likes. The unit
tests feed those decoders bytes chosen by a person, which finds the cases a person thought of, and the premise
of the whole project is that the interesting failures are the ones nobody thought of.

## Running it

```sh
cmake -S . -B build/fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDFR_ASSERTIONS=paranoid
cmake --build build/fuzz -j8
./build/fuzz/fuzz/fuzz_deep --seed 1 --rounds 250000 fuzz/corpus/deep/*
```

Six targets: `iextp`, `moldudp64`, `soupbintcp`, `ouch`, `deep`, `capture`. Address and undefined sanitizers are
always on: a fuzzer without them checks that a decoder does not segfault, which is the least interesting of its
three jobs.

## Two drivers, and why both

libFuzzer's runtime is not shipped with every toolchain; it is absent from the Xcode clang this project is
developed on. "We fuzz in CI" is a poor answer when the person writing the decoder cannot run it. So the targets
are plain `LLVMFuzzerTestOneInput` functions and there are two ways to drive them:

- **libFuzzer**, coverage-guided, in CI on Linux, where it explores far better than anything hand-written;
- **`fuzz/driver.cpp`**, everywhere, seeded and therefore reproducible.

The second is not a poor imitation of the first. Coverage-guided fuzzing finds more, at inputs nobody can
reconstruct without the corpus file. The portable driver finds less, and every finding is **a seed plus a round
number**: the same property the rest of the project rests on, which means a failure is handed over as two
numbers instead of an attachment.

Its mutations are deliberately crude: bit flips, byte overwrites, truncations, growth, and a table of
*interesting lengths*(0, 1, 0x7F, 0xFF, 0x7FFF, 0xFFFF, 0xFFFFFFFF) written into the input where a length
field might be. A decoder's hostile input is almost never structurally novel. It is a valid packet with a wrong
length, which is what these produce directly instead of waiting to stumble on.

## The corpus is real packets

`scripts/fuzz-corpus.sh <capture.pcap>` extracts it from an IEX HIST file, at three layers, because a fuzzer
starting from a whole file exercises the pcap reader and never reaches a message decoder, and one starting from a
single message never exercises framing:

- file prefixes, so the pcap reader meets a truncated header and a truncated record;
- IEX-TP datagrams, for the framing and chain layer;
- individual DEEP messages, for the message decoders.

285 files, 1.1 MB, committed. Every mutation therefore starts from a packet that **actually parses**, which is
where the interesting failures are: not random noise that every decoder rejects at the first byte.

Two of the corpora are deliberately the wrong protocol: the OUCH and SoupBinTCP targets are seeded with DEEP
messages, because well-formed bytes from *another* protocol are exactly the shape of input a dispatcher hands
the wrong decoder when a type byte is hostile.

## The three checks

`fuzz/checks.hpp` holds all of them in one file, so it is possible to see that they are the same three
everywhere. In increasing order of interest:

1. **No crash, no undefined behaviour.** What a fuzzer is usually bought for, and the weakest thing it says.
2. **A success must be self-consistent.** If a decoder reports a frame of N bytes, N is within the input. If it
   hands back a view, the view lies inside the input. *A decoder returning a length it did not have is how a
   caller reads somebody else's memory legitimately*: pointer arithmetic correct, length a lie, sanitizer
   silent.
3. **Framing must be total.** Walking a stream either consumes it or stops; it never loops without advancing.
   An unbounded loop on hostile input is a denial of service no memory checker reports.

Nothing here asserts what a decode *means*. The unit tests do that against real capture bytes; a fuzzer
asserting semantics would mostly assert the fuzzer's own idea of the protocol.

## What it found

**Nothing, across 6 million mutations**: six targets, four seeds each, 250,000 rounds per seed, with ASan and
UBSan on.

That sentence is worth nothing on its own, so it was checked. The length check in the DEEP header decoder was
deliberately removed, making the decoder genuinely wrong: a truncated message would then be decoded against the
full layout and read past its end. The fuzzer found it, and the wording of the failure matters:

```
fuzz invariant broken: a DEEP header succeeded on a length that does not match its type
```

Caught by **check 2**, not by AddressSanitizer. The sanitizer would have caught the read only if the truncated
message happened to sit at the end of an allocation; the self-consistency check catches it every time, from the
first mutation that produces a short message. That is the argument for the second and third checks existing at
all, and it is why "no crashes" was never the claim.

## What this does not cover

- **The recovery state machine.** These targets fuzz decoders. A fuzzer over `recovery::client` would need to
  generate coherent sequences of *calls*, not bytes: a different and larger piece of work, and the deterministic
  simulation tests are what covers it today.
- **Anything past the decode.** A message that decodes correctly and means something absurd is the unit tests'
  problem, and the book oracle's.
- **The corpus is one day of one feed.** IEX DEEP on 2017-08-26, a Saturday test session. A weekday file, or
  another venue, would have shapes this corpus does not.

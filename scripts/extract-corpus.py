#!/usr/bin/env python3
"""Pulls a fuzzing corpus out of a pcap: whole packets, IEX-TP payloads, and single DEEP messages.

Three levels, because a fuzzer starting from a whole file exercises the pcap reader and never reaches a message
decoder, and one starting from a message never exercises framing. Each decoder gets a corpus at its own layer.

Every Nth rather than the first N, so the sample spans the session. Sizes capped so the corpus stays small
enough to commit: a fuzzer's corpus is seed material, not a dataset.
"""
import os
import struct
import sys

capture, out_root = sys.argv[1], sys.argv[2]
data = open(capture, "rb").read()

dirs = {name: os.path.join(out_root, name) for name in ("capture", "iextp", "deep", "moldudp64", "soupbintcp", "ouch")}
for d in dirs.values():
    os.makedirs(d, exist_ok=True)

# The pcap layer: prefixes of the file, so the reader meets a truncated header and a truncated record.
# Capped at 8 KB: a corpus is seed material, not a dataset, and the pcap reader's interesting cases are all in
# the first record rather than the thousandth.
for size in (24, 40, 128, 1024, 8192):
    open(os.path.join(dirs["capture"], f"prefix-{size}"), "wb").write(data[:size])

off, packets, kept_tp, kept_deep = 24, 0, 0, 0
while off + 16 <= len(data):
    _, _, cap, _ = struct.unpack("<IIII", data[off:off + 16])
    off += 16
    frame = data[off:off + cap]
    off += cap
    if len(frame) < 34:
        continue
    et = struct.unpack(">H", frame[12:14])[0]
    p = 14
    if et == 0x8100:
        et = struct.unpack(">H", frame[16:18])[0]
        p = 18
    if et != 0x0800:
        continue
    ihl = (frame[p] & 0xF) * 4
    payload = frame[p + ihl + 8:]
    if len(payload) < 40:
        continue
    packets += 1

    # Every 40th IEX-TP packet, and every packet that carries messages regardless of the stride, up to a cap.
    count = struct.unpack("<H", payload[14:16])[0]
    if packets % 200 == 0 or (count > 0 and kept_tp < 60):
        open(os.path.join(dirs["iextp"], f"pkt-{packets}"), "wb").write(payload)
        # MoldUDP64 and IEX-TP are different protocols over the same shape of datagram, so an IEX-TP packet is
        # useful hostile input for the MoldUDP64 framer: it is well formed and means something else.
        if packets % 1000 == 0:
            open(os.path.join(dirs["moldudp64"], f"foreign-{packets}"), "wb").write(payload)
        kept_tp += 1

    # Individual DEEP messages, one of each type plus a sample.
    q = 40
    for _ in range(count):
        if q + 2 > len(payload):
            break
        ln = struct.unpack("<H", payload[q:q + 2])[0]
        q += 2
        body = payload[q:q + ln]
        q += ln
        if not body or kept_deep >= 90:
            continue
        name = f"msg-{chr(body[0]) if 32 <= body[0] < 127 else body[0]}-{kept_deep}"
        open(os.path.join(dirs["deep"], name), "wb").write(body)
        # OUCH and SoupBinTCP have no capture here, so they get DEEP messages: well-formed bytes from another
        # protocol, which is the shape of input a dispatcher hands the wrong decoder.
        if kept_deep % 20 == 0:
            open(os.path.join(dirs["ouch"], name), "wb").write(body)
            open(os.path.join(dirs["soupbintcp"], name), "wb").write(body)
        kept_deep += 1

print(f"extract-corpus: {packets} packets read, {kept_tp} IEX-TP and {kept_deep} DEEP kept")

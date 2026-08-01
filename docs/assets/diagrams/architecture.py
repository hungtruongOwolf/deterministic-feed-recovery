import pathlib
import sys
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from _diagram import *

W, H = 1720, 1000
parts = []

# ---- containers ----
venue_c = Container(40, 40, 560, 340, "dfr::venue  (the mock exchange)")
chaos_c = Container(660, 40, 300, 170, "dfr::chaos")
recovery_c = Container(660, 400, 640, 400, "dfr::recovery")
book_c = Container(1360, 400, 300, 120, "dfr::book")
trace_c = Container(1360, 560, 300, 120, "dfr::trace")

parts += [venue_c.svg(), chaos_c.svg(), recovery_c.svg(), book_c.svg(), trace_c.svg()]

# ---- venue sub-boxes: publisher/session on top row, retransmit/snapshot on bottom row ----
publisher = make_box(70, 90, ["iextp_publisher", "market data out"])
session = make_box(360, 90, ["order_session", "OUCH in"])
retransmit = make_box(70, 250, ["retransmit_facility", "can refuse"])
snapshot = make_box(360, 250, ["snapshot_facility", "Glimpse"])
for b in (publisher, session, retransmit, snapshot):
    parts.append(b.svg())

# ---- chaos sub-box ----
injector = make_box(700, 90, ["injector<Target>", "seeded, protocol-aware"], w=220)
parts.append(injector.svg())

# ---- recovery sub-boxes ----
client = make_box(700, 500, ["client<Clock>"], w=160, h=220)
arbiter = make_box(940, 440, ["arbiter", "A/B merge"], w=180)
gap_tracker = make_box(940, 510, ["gap_tracker"], w=180)
requester = make_box(940, 580, ["requester"], w=180)
replay = make_box(940, 650, ["replay_buffer"], w=180)
for b in (client, arbiter, gap_tracker, requester, replay):
    parts.append(b.svg())

# ---- book / trace / viewer ----
order_book = make_box(1390, 440, ["order_book<N>"], w=240)
recorder = make_box(1390, 600, ["recorder", "JSONL, one line/event"], w=240)
viewer = make_box(1390, 760, ["viewer/", "WASM + React"], w=240, fill=PAGE, stroke=INK_SOFT)
parts += [order_book.svg(), recorder.svg(), viewer.svg()]

# ---- markers ----
colors = {INK_SOFT.lstrip("#"): INK_SOFT, FAULT.lstrip("#"): FAULT, RECOVERY.lstrip("#"): RECOVERY}
markers = arrow_marker_defs(colors)

# ---- arrows ----
# venue -> chaos: publisher's market data (straight, top row only, nothing else shares that row)
parts.append(arrow(publisher.anchor("right"), injector.anchor("left"),
                    label="market data out\\n(IEX-TP/MoldUDP64)", color=INK_SOFT, label_pos=0.82))

# chaos -> venue.session: routed below the top row entirely, so it cannot cross the publisher arrow
# or the retransmit/snapshot row.
parts.append(path_arrow(
    [(injector.x, injector.y + injector.h * 0.7), (630, injector.y + injector.h * 0.7),
     (630, 205), (session.x + session.w / 2, 205), (session.x + session.w / 2, session.y + session.h)],
    color=INK_SOFT, label="orders in, replies out\\n(OUCH/SoupBinTCP)", label_at=(630, 185),
))

# chaos -> recovery (the damaged stream) and chaos -> trace (every fault)
parts.append(arrow(injector.anchor("bottom"), client.anchor("top"),
                    label="damaged stream", color=FAULT, width=2.2))
parts.append(path_arrow(
    [(injector.x + injector.w, injector.cy), (1610, injector.cy), (1610, recorder.y)],
    color=FAULT, label="every fault", label_at=(1610, 260),
))

# recovery client -> its own pieces
parts.append(arrow(client.anchor("right"), arbiter.anchor("left"), color=INK_SOFT))
parts.append(arrow(client.anchor("right"), gap_tracker.anchor("left"), color=INK_SOFT))
parts.append(arrow(client.anchor("right"), requester.anchor("left"), color=INK_SOFT))
parts.append(arrow(client.anchor("right"), replay.anchor("left"), color=INK_SOFT))

# requester <-> retransmit, replay <-> snapshot: routed through the gap between the two containers
# (x 600-660), at the height of retransmit/snapshot's own row, well clear of publisher/session above.
gap_x = 625
parts.append(path_arrow(
    [(retransmit.x + retransmit.w, retransmit.cy), (gap_x, retransmit.cy),
     (gap_x, requester.cy - 30), (gap_x, requester.cy), (recovery_c.x, requester.cy)],
    color=RECOVERY, label="request\\n<->\\nfill/refuse", label_at=(gap_x, requester.cy - 55),
))
parts.append(path_arrow(
    [(snapshot.x + snapshot.w, snapshot.cy), (gap_x + 15, snapshot.cy),
     (gap_x + 15, replay.cy), (recovery_c.x, replay.cy)],
    color=RECOVERY, label="begin/levels\\n<->\\nend", label_at=(gap_x + 15, replay.cy - 55),
))

# recovery -> book, recovery -> trace
parts.append(path_arrow(
    [(recovery_c.x + recovery_c.w, arbiter.cy), (order_book.x, order_book.cy)],
    color=INK_SOFT, label="sequence-ordered", label_at=((recovery_c.x + recovery_c.w + order_book.x) / 2, arbiter.cy - 10),
))
parts.append(path_arrow(
    [(recovery_c.x + recovery_c.w, replay.cy), (recorder.x, recorder.cy)],
    color=INK_SOFT, label="every decision", label_at=((recovery_c.x + recovery_c.w + recorder.x) / 2, replay.cy - 10),
))
parts.append(arrow(recorder.anchor("bottom"), viewer.anchor("top"), label="run.jsonl", color=INK_SOFT))

body = "\n".join(parts)
doc = svg_document(W, H, body, markers)
open(HERE / "architecture.svg", "w").write(doc)
print("wrote architecture.svg")

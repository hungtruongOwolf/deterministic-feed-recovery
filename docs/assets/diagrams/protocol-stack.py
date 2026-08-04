import pathlib
import sys
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from _diagram import *

W, H = 1500, 520
parts = []

parts.append(text(40, 34, "dfr::wire  -  seven protocols, three transports", size=17, weight="600"))
parts.append(text(40, 56, "each message layer is decoded independently of the transport that carries it; only the umbrella",
                   size=12.5, color=INK_SOFT, italic=True))
parts.append(text(40, 72, "header for each transport knows which message headers ride on it.",
                   size=12.5, color=INK_SOFT, italic=True))

# ---- message layer (top row) ----
deep = make_box(80, 120, ["deep/", "DEEP 1.0", "market data, IEX"], w=260)
itch = make_box(400, 120, ["itch/", "TotalView-ITCH 5.0", "order-level market data"], w=260)
ouch = make_box(760, 120, ["ouch/", "OUCH 4.2", "order entry, both directions"], w=260)
glimpse = make_box(1080, 120, ["glimpse/", "snapshot protocol", "begin / levels / end"], w=260)
for b in (deep, itch, ouch, glimpse):
    parts.append(b.svg())

# ---- transport layer (bottom row) ----
iextp = make_box(80, 320, ["iextp/", "IEX-TP", "UDP multicast, 40-byte header"], w=260, fill=PAGE, stroke=INK_SOFT)
moldudp64 = make_box(400, 320, ["moldudp64/", "MoldUDP64", "UDP multicast, 20-byte header"], w=260, fill=PAGE, stroke=INK_SOFT)
soupbintcp = make_box(870, 320, ["soupbintcp/", "SoupBinTCP 3.00", "TCP session, login/logout framed"], w=470, fill=PAGE, stroke=INK_SOFT)
for b in (iextp, moldudp64, soupbintcp):
    parts.append(b.svg())

colors = {INK_SOFT.lstrip("#"): INK_SOFT}
markers = arrow_marker_defs(colors)

parts.append(arrow(deep.anchor("bottom"), iextp.anchor("top"), label="rides on", color=INK_SOFT))
parts.append(arrow(itch.anchor("bottom"), moldudp64.anchor("top"), label="rides on", color=INK_SOFT))
parts.append(arrow(ouch.anchor("bottom"), (ouch.cx, soupbintcp.y), label="rides on", color=INK_SOFT))
parts.append(arrow(glimpse.anchor("bottom"), (glimpse.cx, soupbintcp.y), label="rides on", color=INK_SOFT))

note = make_box(80, 420, [
    "The same fault injector attacks both transport shapes: chaos::injector<Target> is a template, and",
    "moldudp64_target / iextp_target are the only two specializations, so every fault op (drop, delay,",
    "duplicate, reorder, corrupt, truncate) is written once and applies to both.",
], w=1260, fill=PAGE, stroke=RULE, bold_first=False, text_color=INK_SOFT)
parts.append(note.svg())

body = "\n".join(parts)
doc = svg_document(W, H, body, markers)
open(HERE / "protocol-stack.svg", "w").write(doc)
print("wrote protocol-stack.svg")

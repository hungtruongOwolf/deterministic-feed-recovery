import pathlib
import sys
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from _diagram import *

W, H = 1360, 900
parts = []

parts.append(text(40, 34, "One gap, start to finish", size=17, weight="600"))
parts.append(text(40, 56, "sequence 4 is lost; the client keeps delivering what arrives while it asks for the gap back,",
                   size=12.5, color=INK_SOFT, italic=True))
parts.append(text(40, 72, "and applies the repair in sequence order, not in the order it arrived.",
                   size=12.5, color=INK_SOFT, italic=True))

lanes = {
    "venue": 140,
    "wire": 420,
    "client": 700,
    "book": 1080,
}
labels = {
    "venue": ["dfr::venue"],
    "wire": ["the wire", "(damaged)"],
    "client": ["dfr::recovery::client", "(gap_tracker + requester + arbiter)"],
    "book": ["dfr::book", "(downstream)"],
}
top_y, bot_y = 110, 860
for key, x in lanes.items():
    parts.append(path_arrow([(x, top_y + 50), (x, bot_y)], color=RULE, width=1.2, dash="2,4"))
    b = make_box(x - 130, top_y, labels[key], w=260, fill=PAGE, stroke=INK_SOFT)
    parts.append(b.svg())

def event(y, x, s, color=INK_SOFT, anchor="middle", dx=0):
    return text(x + dx, y, s, size=12.5, color=color, anchor=anchor)

def hop(y, x1, x2, s, color=INK_SOFT, label_dy=-8):
    parts.append(arrow((x1, y), (x2, y), color=color))
    mid = (x1 + x2) / 2
    parts.append(text(mid, y + label_dy, s, size=11.5, color=color, anchor="middle", italic=True))

y = 190
hop(y, lanes["venue"], lanes["wire"], "sequence 1, 2, 3")
y += 20
hop(y, lanes["wire"], lanes["client"], "1, 2, 3 arrive")
y += 55
parts.append(text(lanes["wire"], y, "sequence 4", size=12, color=FAULT, anchor="middle", weight="600"))
parts.append(text(lanes["wire"], y + 16, "dropped by dfr::chaos", size=11, color=FAULT, anchor="middle", italic=True))
y += 55
hop(y, lanes["venue"], lanes["wire"], "sequence 5")
y += 20
hop(y, lanes["wire"], lanes["client"], "5 arrives: gap_tracker sees 4 missing")
y += 45
hop(y, lanes["client"], lanes["book"], "1, 2, 3 delivered", color=HEALTHY)
y += 20
hop(y, lanes["client"], lanes["book"], "5 delivered too: stalling on a hole turns one loss into an outage",
    color=HEALTHY, label_dy=14)
y += 55
hop(y, lanes["client"], lanes["venue"], "requester: retransmit sequence 4", color=RECOVERY)
y += 45
hop(y, lanes["venue"], lanes["client"], "retransmit_facility: here is 4", color=RECOVERY)
y += 45
parts.append(text(lanes["client"], y, "arbiter reorders: 4 is applied between 3 and 5,",
                   size=12, color=INK, anchor="middle", weight="600"))
parts.append(text(lanes["client"], y + 16, "not appended after 5", size=12, color=INK, anchor="middle", weight="600"))
y += 50
hop(y, lanes["client"], lanes["book"], "4 delivered, in sequence order", color=HEALTHY)

note = make_box(40, 800, [
    "Delivering 4 after 5 in arrival order, instead of between 3 and 5 in sequence order, is the exact",
    "defect book_oracle_test.cpp exists to catch: an aggregated book is last-write-wins, so the wrong order",
    "leaves the wrong size at that price, permanently.",
], w=1280, fill=PAGE, stroke=RULE, bold_first=False, text_color=INK_SOFT)
parts.append(note.svg())

colors = {INK_SOFT.lstrip("#"): INK_SOFT, HEALTHY.lstrip("#"): HEALTHY, RECOVERY.lstrip("#"): RECOVERY,
          FAULT.lstrip("#"): FAULT, INK.lstrip("#"): INK}
markers = arrow_marker_defs(colors)

body = "\n".join(parts)
doc = svg_document(W, H + 60, body, markers)
open(HERE / "recovery-sequence.svg", "w").write(doc)
print("wrote recovery-sequence.svg")

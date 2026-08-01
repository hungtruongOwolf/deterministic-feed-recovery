import pathlib
import sys
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from _diagram import *

W, H = 1180, 900
parts = []

parts.append(text(40, 34, "Three defences, escalating", size=17, weight="600"))
parts.append(text(40, 56, "each is reached only when the layer above it could not help; a packet descends the ladder,",
                   size=12.5, color=INK_SOFT, italic=True))
parts.append(text(40, 72, "it does not choose a rung.", size=12.5, color=INK_SOFT, italic=True))

# Layer 1: two lines
l1 = Container(40, 110, 1100, 160, "1  TWO LINES CARRY THE FEED")
line_a = make_box(90, 165, ["line A"], w=460, h=40, fill=PAGE, stroke=INK_SOFT)
line_b = make_box(90, 215, ["line B"], w=460, h=40, fill=PAGE, stroke=INK_SOFT)
receiver = make_box(620, 165, ["arbiter", "merges A and B by sequence,", "discards the duplicate"], w=460, h=90)
parts += [l1.svg(), line_a.svg(), line_b.svg(), receiver.svg()]
parts.append(arrow(line_a.anchor("right"), receiver.anchor("left"), color=INK_SOFT))
parts.append(arrow(line_b.anchor("right"), receiver.anchor("left"), color=INK_SOFT))
parts.append(text(1140, 200, "costs bandwidth all day", size=12, color=INK_SOFT, italic=True, anchor="end"))
parts.append(text(1140, 216, "costs no time at all", size=12, color=INK_SOFT, italic=True, anchor="end"))

parts.append(path_arrow([(590, 270), (590, 320)], color=RULE_STRONG, width=2,
                          label="escalates when the layer above cannot help", label_at=(590, 300)))

# Layer 2: retransmit
l2 = Container(40, 330, 1100, 160, "2  ASK FOR IT BACK")
retransmit = make_box(90, 390, ["requester", "retransmission request,", "chunked and re-asked on timeout"], w=460, h=90)
facility = make_box(620, 390, ["retransmit_facility", "fills the range, or refuses", "once the retention window has passed"], w=460, h=90)
parts += [l2.svg(), retransmit.svg(), facility.svg()]
parts.append(arrow(retransmit.anchor("right"), facility.anchor("left"),
                    label="request  <->  fill or refuse", color=RECOVERY))
parts.append(text(1140, 420, "costs a round trip", size=12, color=INK_SOFT, italic=True, anchor="end"))
parts.append(text(1140, 436, "and it expires", size=12, color=INK_SOFT, italic=True, anchor="end"))

parts.append(path_arrow([(590, 490), (590, 540)], color=RULE_STRONG, width=2,
                          label="escalates when the layer above cannot help", label_at=(590, 520)))

# Layer 3: snapshot
l3 = Container(40, 550, 1100, 160, "3  REBUILD FROM A SNAPSHOT")
replay = make_box(90, 610, ["replay_buffer", "a client with no state at all", "rebuilds the whole book"], w=460, h=90)
snap = make_box(620, 610, ["snapshot_facility", "streams every price level,", "framed as its own numbered stream"], w=460, h=90)
parts += [l3.svg(), replay.svg(), snap.svg()]
parts.append(arrow(replay.anchor("right"), snap.anchor("left"),
                    label="begin  <->  levels  <->  end", color=RECOVERY))
parts.append(text(1140, 640, "costs seconds", size=12, color=INK_SOFT, italic=True, anchor="end"))
parts.append(text(1140, 656, "blind while it runs", size=12, color=INK_SOFT, italic=True, anchor="end"))

note = make_box(40, 750, [
    "The last row is the interesting one: the client's own book is not trusted while a snapshot runs, and",
    "unfillable_messages says so rather than publishing a book that looks complete and is permanently wrong.",
], w=1100, fill=PAGE, stroke=RULE, bold_first=False, text_color=INK_SOFT)
parts.append(note.svg())

colors = {INK_SOFT.lstrip("#"): INK_SOFT, RECOVERY.lstrip("#"): RECOVERY}
markers = arrow_marker_defs(colors)

body = "\n".join(parts)
doc = svg_document(W, H, body, markers)
open(HERE / "defense-layers.svg", "w").write(doc)
print("wrote defense-layers.svg")

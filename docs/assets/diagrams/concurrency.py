import pathlib
import sys
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from _diagram import *

W, H = 1580, 560
parts = []

producer_c = Container(40, 40, 420, 300, "one thread  ·  deterministic core")
ring_c = Container(560, 40, 340, 460, "dfr::concurrent::spsc_ring")
consumer_c = Container(1000, 40, 460, 300, "another thread  ·  the strategy")

parts += [producer_c.svg(), ring_c.svg(), consumer_c.svg()]

client = make_box(70, 90, ["dfr::recovery::client"], w=360)
push = make_box(70, 170, ["push(delivery)", "refused if full, never overwrites"], w=360)
parts += [client.svg(), push.svg()]

tail = make_box(600, 90, ["tail_, advanced by push()", "its own cache line"], w=260)
slots = make_box(600, 200, ["fixed-size delivery slots", "flat, memcpy'd in, no pointers"], w=260, h=90)
head = make_box(600, 340, ["head_, advanced by pop()", "its own cache line"], w=260)
parts += [tail.svg(), slots.svg(), head.svg()]

pop = make_box(1030, 90, ["pop() -> delivery"], w=200)
strategy = make_box(1270, 90, ["whatever a caller", "wants to run on it"], w=170)
parts += [pop.svg(), strategy.svg()]

colors = {INK_SOFT.lstrip("#"): INK_SOFT, RULE.lstrip("#"): RULE}
markers = arrow_marker_defs(colors)

parts.append(arrow(client.anchor("bottom"), push.anchor("top"), color=INK_SOFT))
parts.append(arrow(push.anchor("right"), slots.anchor("left"), label="release store", color=INK_SOFT))
parts.append(arrow(slots.anchor("right"), pop.anchor("left"), label="acquire load", color=INK_SOFT))
parts.append(arrow(pop.anchor("right"), strategy.anchor("left"), color=INK_SOFT))

note = make_box(560, 460, [
    "TSan alone passed a version with the release/acquire",
    "relaxed to weaker orders. An arm64 property test",
    "caught it 12/12 runs; TSan does not model ordering,",
    "only the presence of a race. docs/CONCURRENCY.md",
], w=760, fill=PAGE, stroke=RULE, bold_first=False, text_color=INK_SOFT)
parts.append(note.svg())
parts.append(path_arrow([(head.x, head.y + head.h / 2), (head.x - 30, head.y + head.h / 2),
                          (head.x - 30, note.y + 10), (note.x + 10, note.y + 10)],
                         color=RULE, dash="4,3"))

body = "\n".join(parts)
doc = svg_document(W, H + 100, body, markers)
open(HERE / "concurrency.svg", "w").write(doc)
print("wrote concurrency.svg")

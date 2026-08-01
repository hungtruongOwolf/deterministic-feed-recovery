"""A tiny hand-rolled SVG diagram helper: boxes, containers, arrows, labels.

No external diagramming tool. Every position is computed from what you ask for (box width/height
from its text, arrow endpoints from box edges), matching the project's own "nothing is placed by
eye" philosophy for the viewer's own drawings.

Palette matches viewer/src/ui/theme.css exactly.
"""
from __future__ import annotations
from dataclasses import dataclass, field

PAGE = "#f1ece1"
PAGE_SUNK = "#e7e0d2"
INK = "#1b1a17"
INK_SOFT = "#6d675c"
RULE = "#cfc7b5"
RULE_STRONG = "#92876f"
FAULT = "#a8452c"
RECOVERY = "#2b59d1"
UNFILLABLE = "#7a1717"
HEALTHY = "#3c6b4a"
MONO = "ui-monospace, SFMono-Regular, 'JetBrains Mono', Menlo, monospace"

CHAR_W = 7.1        # advance width of MONO at font-size 13
LINE_H = 17
PAD_X = 14
PAD_Y = 10


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


@dataclass
class Box:
    x: float
    y: float
    w: float
    h: float
    lines: list[str]
    fill: str = PAGE_SUNK
    stroke: str = RULE_STRONG
    stroke_width: float = 1.4
    bold_first: bool = True
    text_color: str = INK
    dash: str | None = None
    corner: float = 3

    @property
    def cx(self) -> float:
        return self.x + self.w / 2

    @property
    def cy(self) -> float:
        return self.y + self.h / 2

    def anchor(self, side: str) -> tuple[float, float]:
        if side == "top":
            return (self.cx, self.y)
        if side == "bottom":
            return (self.cx, self.y + self.h)
        if side == "left":
            return (self.x, self.cy)
        if side == "right":
            return (self.x + self.w, self.cy)
        raise ValueError(side)

    def svg(self) -> str:
        dash = f' stroke-dasharray="{self.dash}"' if self.dash else ""
        out = [
            f'<rect x="{self.x:.1f}" y="{self.y:.1f}" width="{self.w:.1f}" height="{self.h:.1f}" '
            f'rx="{self.corner}" fill="{self.fill}" stroke="{self.stroke}" '
            f'stroke-width="{self.stroke_width}"{dash}/>'
        ]
        n = len(self.lines)
        block_h = n * LINE_H
        top = self.cy - block_h / 2 + LINE_H * 0.78
        for i, line in enumerate(self.lines):
            weight = "600" if (self.bold_first and i == 0) else "400"
            size = 13.5 if (self.bold_first and i == 0) else 12.5
            out.append(
                f'<text x="{self.cx:.1f}" y="{top + i * LINE_H:.1f}" text-anchor="middle" '
                f'font-family="{MONO}" font-size="{size}" font-weight="{weight}" '
                f'fill="{self.text_color}">{esc(line)}</text>'
            )
        return "\n".join(out)


def make_box(x: float, y: float, lines: list[str], w: float | None = None, h: float | None = None,
             **kw) -> Box:
    if w is None:
        longest = max((len(l) for l in lines), default=0)
        w = longest * CHAR_W + 2 * PAD_X
    if h is None:
        h = len(lines) * LINE_H + 2 * PAD_Y - 4
    return Box(x, y, w, h, lines, **kw)


@dataclass
class Container:
    x: float
    y: float
    w: float
    h: float
    title: str
    fill: str = "none"
    stroke: str = RULE_STRONG
    title_color: str = INK

    def svg(self) -> str:
        out = [
            f'<rect x="{self.x:.1f}" y="{self.y:.1f}" width="{self.w:.1f}" height="{self.h:.1f}" '
            f'rx="4" fill="{self.fill}" stroke="{self.stroke}" stroke-width="1.2" stroke-dasharray="3,3"/>',
            f'<text x="{self.x + 12:.1f}" y="{self.y + 20:.1f}" font-family="{MONO}" font-size="14" '
            f'font-weight="600" fill="{self.title_color}">{esc(self.title)}</text>',
        ]
        return "\n".join(out)


def arrow_marker_defs(color_ids: dict[str, str]) -> str:
    defs = []
    for name, color in color_ids.items():
        defs.append(
            f'<marker id="arrow-{name}" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" '
            f'markerHeight="7" orient="auto-start-reverse">'
            f'<path d="M 0 0 L 10 5 L 0 10 z" fill="{color}"/></marker>'
        )
    return "\n".join(defs)


def elbow_path(p1: tuple[float, float], p2: tuple[float, float], bend: str = "auto") -> str:
    x1, y1 = p1
    x2, y2 = p2
    if bend == "h-first":
        midx = (x1 + x2) / 2
        return f"M {x1:.1f} {y1:.1f} L {midx:.1f} {y1:.1f} L {midx:.1f} {y2:.1f} L {x2:.1f} {y2:.1f}"
    if bend == "v-first":
        midy = (y1 + y2) / 2
        return f"M {x1:.1f} {y1:.1f} L {x1:.1f} {midy:.1f} L {x2:.1f} {midy:.1f} L {x2:.1f} {y2:.1f}"
    return f"M {x1:.1f} {y1:.1f} L {x2:.1f} {y2:.1f}"


def path_arrow(points: list[tuple[float, float]], color=INK_SOFT, label: str | None = None,
                label_at: tuple[float, float] | None = None, width: float = 1.6,
                label_bg: str = PAGE, dash: str | None = None) -> str:
    d = "M " + " L ".join(f"{x:.1f} {y:.1f}" for x, y in points)
    dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
    out = [
        f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}"{dash_attr} '
        f'marker-end="url(#arrow-{color.lstrip(chr(35))})"/>'
    ]
    if label:
        lx, ly = label_at if label_at else points[len(points) // 2]
        words = label.split("\\n")
        for i, w in enumerate(words):
            out.append(
                f'<rect x="{lx - len(w) * 3.6 - 4:.1f}" y="{ly - 11 + i * 14:.1f}" '
                f'width="{len(w) * 7.2 + 8:.1f}" height="14" fill="{label_bg}" opacity="0.9"/>'
            )
            out.append(
                f'<text x="{lx:.1f}" y="{ly + 2 + i * 14:.1f}" text-anchor="middle" '
                f'font-family="{MONO}" font-size="11.5" font-style="italic" '
                f'fill="{color}">{esc(w)}</text>'
            )
    return "\n".join(out)


def arrow(p1, p2, color=INK_SOFT, label: str | None = None, label_pos: float = 0.5,
          dash: str | None = None, bend: str = "straight", width: float = 1.6,
          label_bg: str = PAGE, curved: bool = False) -> str:
    if bend == "straight" and not curved:
        d = f"M {p1[0]:.1f} {p1[1]:.1f} L {p2[0]:.1f} {p2[1]:.1f}"
        lx, ly = p1[0] + (p2[0] - p1[0]) * label_pos, p1[1] + (p2[1] - p1[1]) * label_pos
    elif curved:
        mx, my = (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2
        d = f"M {p1[0]:.1f} {p1[1]:.1f} Q {mx:.1f} {my - 26:.1f} {p2[0]:.1f} {p2[1]:.1f}"
        lx, ly = mx, my - 26
    else:
        d = elbow_path(p1, p2, bend)
        lx, ly = (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2 if bend != "h-first" else p1[1]
        if bend == "h-first":
            lx, ly = (p1[0] + p2[0]) / 2, p1[1] - 6
        else:
            lx, ly = p1[0] + 6, (p1[1] + p2[1]) / 2
    dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
    out = [
        f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}"{dash_attr} '
        f'marker-end="url(#arrow-{color.lstrip(chr(35))})"/>'
    ]
    if label:
        words = label.split("\\n")
        for i, w in enumerate(words):
            out.append(
                f'<rect x="{lx - len(w) * 3.6 - 4:.1f}" y="{ly - 11 + i * 14:.1f}" '
                f'width="{len(w) * 7.2 + 8:.1f}" height="14" fill="{label_bg}" opacity="0.9"/>'
            )
            out.append(
                f'<text x="{lx:.1f}" y="{ly + 2 + i * 14:.1f}" text-anchor="middle" '
                f'font-family="{MONO}" font-size="11.5" font-style="italic" '
                f'fill="{color}">{esc(w)}</text>'
            )
    return "\n".join(out)


def text(x, y, s, size=13, weight="400", color=INK, anchor="start", italic=False,
          family=MONO) -> str:
    style = ' font-style="italic"' if italic else ""
    return (f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" font-family="{family}" '
            f'font-size="{size}" font-weight="{weight}" fill="{color}"{style}>{esc(s)}</text>')


def svg_document(width: float, height: float, body: str, markers: str = "") -> str:
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.0f} {height:.0f}" width="{width:.0f}" height="{height:.0f}">
  <defs>
{markers}
  </defs>
  <rect x="0" y="0" width="{width:.0f}" height="{height:.0f}" fill="{PAGE}"/>
{body}
</svg>
'''

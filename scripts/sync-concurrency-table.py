#!/usr/bin/env python3
"""Regenerates the "What it costs" table in docs/CONCURRENCY.md from bench/handoff.json.

The table drifted once already: a benchmark rerun changed every row, the doc's prose was updated by hand,
and the table was not, so it disagreed with the very JSON it claims to summarize. This is the same class of
defect docs/BENCHMARKS.md's table already guards against with scripts/sync-benchmark-table.py; this script is
that guard's counterpart for the concurrency table, which has a different shape (ns per message, messages/s,
and a refusal count, rather than three assertion levels side by side).

The prose around the table is left alone, same reasoning as the benchmark table's script: generating the
argument would mean the argument could not be edited.
"""
import json
import pathlib
import sys

here = pathlib.Path(__file__).resolve().parent.parent
data = json.load(open(here / "bench/handoff.json"))
doc = here / "docs/CONCURRENCY.md"

by_name = {m["name"]: m for m in data["measurements"]}

rows = [
    ("272-byte records, one `pop` each", "one at a time", False),
    ("272-byte records, `pop_batch` of 64", "drained in batches of 64", True),
    ("8-byte records, padded indices", "8-byte records, padded", False),
    ("8-byte records, indices sharing a line", "8-byte records, unpadded", False),
    ("consumer deliberately too slow", "consumer falling behind", False),
]


def fmt_rate(per_second):
    millions = per_second / 1_000_000
    return f"{millions:.1f} M" if millions < 100 else f"{millions:.0f} M"


lines = ["| | ns per message | messages/s |", "|---|---|---|"]
for label, key, bold in rows:
    m = by_name[key]
    ns = f"{m['ns_per_message']:.1f}"
    rate = fmt_rate(m["messages_per_second"])
    if key == "consumer falling behind":
        refused_m = m["refused"] / 1_000_000
        cell = f"~{ns}", f"~{rate}, **{refused_m:.2f} M refused**"
    elif bold:
        cell = f"**~{ns}**", f"**~{rate}**"
    else:
        cell = f"~{ns}", f"~{rate}"
    lines.append(f"| {label} | {cell[0]} | {cell[1]} |")
table = "\n".join(lines)

text = doc.read_text()
start = text.index("| | ns per message |")
end = text.index("\n\n", start)
new_text = text[:start] + table + text[end:]

if "--check" in sys.argv:
    sys.exit(0 if new_text == text else 1)

doc.write_text(new_text)
print("sync-concurrency-table: docs/CONCURRENCY.md now matches bench/handoff.json")

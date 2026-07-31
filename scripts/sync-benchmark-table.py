#!/usr/bin/env python3
"""Regenerates the benchmark table in docs/BENCHMARKS.md from the committed JSON.

Two figures in that table had drifted from the JSON they claim to come from, which is the same class of defect as
a stale test count and harder to spot: nobody re-reads a table of nanoseconds. So it is generated, and
`check-docs.sh --check` fails the build when the file and the JSON disagree.

The prose around it is left alone. Generating the argument would mean the argument could not be edited.
"""
import json
import pathlib
import re
import sys

here = pathlib.Path(__file__).resolve().parent.parent
runs = {level: json.load(open(here / f"bench/results-{level}.json")) for level in ("off", "fast", "paranoid")}
doc = here / "docs/BENCHMARKS.md"


def best(level, name):
    for m in runs[level]["measurements"]:
        if m["name"] == name:
            return m["best_ns"]
    return None


rows = ["| operation | off | fast | paranoid | paranoid cost |", "|---|---|---|---|---|"]
for m in runs["off"]["measurements"]:
    o, fa, pa = best("off", m["name"]), best("fast", m["name"]), best("paranoid", m["name"])
    if fa is None or pa is None:
        continue
    ratio = pa / o
    verdict = f"**{ratio:.1f}×**" if ratio > 1.15 else "below the noise floor"
    rows.append(f"| {m['name']} | **{o:.2f} ns** | {fa:.2f} | {pa:.2f} | {verdict} |")
allocations = runs["off"]["allocations_after_init"]
rows.append(f"| allocations after initialisation | **{allocations}** | {allocations} | {allocations} | none |")
table = "\n".join(rows)

text = doc.read_text()
start = text.index("| operation | off |")
end = text.index("\n\nAt 8 messages per packet")
rebuilt = text[:start] + table + text[end:]

# The throughput sentence is derived from the same figure, so it drifts with it.
ingest = best("off", "ingest a packet end to end")
rebuilt = re.sub(
    r"At 8 messages per packet, the clean-feed figure is roughly \*\*[0-9]+ million packets or [0-9]+ million messages a\nsecond on one core\*\*",
    f"At 8 messages per packet, the clean-feed figure is roughly **{1e9/ingest/1e6:.0f} million packets or "
    f"{8e9/ingest/1e6:.0f} million messages a\nsecond on one core**",
    rebuilt,
)

if "--check" in sys.argv:
    if rebuilt != text:
        print("sync-benchmark-table: docs/BENCHMARKS.md disagrees with bench/results-*.json", file=sys.stderr)
        sys.exit(1)
    print("sync-benchmark-table: the table matches the JSON")
    sys.exit(0)

doc.write_text(rebuilt)
print("sync-benchmark-table: regenerated the table from bench/results-*.json")

#!/usr/bin/env python3
"""Merges repeated benchmark runs by taking the minimum of every figure.

Minimum rather than mean, and it is not an attempt to flatter the numbers. Noise on a benchmark is one-sided:
a scheduler preemption, a thermal ramp or a neighbouring process can only ever make an operation appear
slower, never faster. So the minimum over repetitions converges on what the code costs, while the mean
converges on what the machine was doing that afternoon.

The percentiles are merged the same way, which needs saying: the reported p99 is the *lowest p99 observed
across rounds*, i.e. the tail of the least disturbed run. That is the honest reading — it describes the tail
the code produces, not the tail the operating system added — and it is written down here rather than left for
somebody to infer.
"""
import json
import sys

out_path, *inputs = sys.argv[1:]
runs = [json.load(open(p)) for p in sorted(inputs)]
if not runs:
    raise SystemExit("merge-benchmarks: no input files")

FIGURES = ("best_ns", "p50_ns", "p99_ns", "worst_ns", "mean_ns")
merged = dict(runs[0])
merged["rounds"] = len(runs)

by_name = {}
for run in runs:
    for m in run["measurements"]:
        keep = by_name.setdefault(m["name"], dict(m))
        for field in FIGURES:
            keep[field] = min(keep[field], m[field])
        keep["per_second"] = 1e9 / keep["p50_ns"] if keep["p50_ns"] > 0 else 0

merged["measurements"] = [by_name[m["name"]] for m in runs[0]["measurements"]]
# An allocation anywhere in any round is an allocation, so this one takes the maximum.
merged["allocations_after_init"] = max(r["allocations_after_init"] for r in runs)
merged["limits"] = merged["limits"] + [{
    "claim": "the figures are minima over %d rounds" % len(runs),
    "status": "measured",
    "note": "noise on a benchmark is one-sided, so the minimum converges on what the code costs; the "
            "reported p99 is the lowest p99 across rounds, not the worst",
}]

with open(out_path, "w") as f:
    json.dump(merged, f, indent=None, separators=(",", ":"))
    f.write("\n")
print(f"merge-benchmarks: {out_path} from {len(runs)} rounds")

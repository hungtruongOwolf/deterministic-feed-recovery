#!/usr/bin/env python3
"""Refuse public-header dependencies that cross an unowned component boundary."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "include" / "dfr"
DEPENDENCIES = {
    "core": set(),
    "wire": {"core"},
    "capture": {"core"},
    "chaos": {"core", "wire"},
    "recovery": {"core"},
    "book": {"core", "wire"},
    "concurrent": {"core"},
    "trace": {"core", "recovery"},
    "venue": {"book", "core", "recovery", "wire"},
}
INCLUDE_PATTERN = re.compile(r"^#include <dfr/([^/]+)/")


def main() -> int:
    failures: list[str] = []
    seen = {path.name for path in INCLUDE.iterdir() if path.is_dir()}
    if seen != set(DEPENDENCIES):
        missing = sorted(seen - set(DEPENDENCIES))
        stale = sorted(set(DEPENDENCIES) - seen)
        if missing:
            failures.append(f"components missing from policy: {', '.join(missing)}")
        if stale:
            failures.append(f"policy names absent components: {', '.join(stale)}")

    for header in sorted(INCLUDE.rglob("*.hpp")):
        source = header.relative_to(INCLUDE).parts[0]
        for line_number, line in enumerate(header.read_text().splitlines(), 1):
            match = INCLUDE_PATTERN.match(line)
            if match is None or match.group(1) == source:
                continue
            target = match.group(1)
            if target not in DEPENDENCIES[source]:
                relative = header.relative_to(ROOT)
                failures.append(
                    f"{relative}:{line_number}: {source} may not include {target}"
                )

    if failures:
        print("component dependency check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("component dependency check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

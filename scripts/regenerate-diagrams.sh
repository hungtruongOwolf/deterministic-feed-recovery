#!/usr/bin/env bash
# Regenerates the committed architecture diagrams from their D2 source.
#
# docs/assets/diagrams/*.svg are what README.md and docs/DESIGN.md embed; the .d2 files next to
# them are the actual source. Edit a .d2 file, run this, and `git diff docs/assets/diagrams/` is
# the review: a diagram that changed without a corresponding source edit is a regeneration bug,
# not a design change.
set -euo pipefail

if ! command -v d2 >/dev/null 2>&1; then
  echo "regenerate-diagrams: d2 not found on PATH." >&2
  echo "  install: curl -fsSL https://d2lang.com/install.sh | sh -s --" >&2
  exit 1
fi

dir="docs/assets/diagrams"
for src in "${dir}"/*.d2; do
  out="${src%.d2}.svg"
  d2 --theme 0 "${src}" "${out}"
done

echo "regenerate-diagrams: done; git diff ${dir} is the review"

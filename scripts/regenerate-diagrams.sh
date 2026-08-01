#!/usr/bin/env bash
# Regenerates the committed architecture diagrams from their Python source.
#
# docs/assets/diagrams/*.svg are what README.md embeds; the .py files next to them are the actual
# source, a small hand-rolled SVG helper (docs/assets/diagrams/_diagram.py: boxes, containers,
# arrows, labels) rather than a diagramming tool, so there is nothing to install and nothing that
# renders differently on a different machine. Edit a .py file, run this, and
# `git diff docs/assets/diagrams/` is the review: a diagram that changed without a corresponding
# source edit is a regeneration bug, not a design change.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="${here}/docs/assets/diagrams"

for src in "${dir}"/*.py; do
  name="$(basename "${src}")"
  [[ "${name}" == _* ]] && continue   # _diagram.py is the shared helper, not a diagram of its own
  python3 "${src}"
done

echo "regenerate-diagrams: done; git diff ${dir} is the review"

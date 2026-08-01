#!/usr/bin/env bash
# Extracts a fuzzing corpus from a real capture.
#
# A corpus of hand-written packets fuzzes the cases somebody imagined. A corpus of real ones starts every
# mutation from a packet that actually parses, which is where the interesting failures are: a valid packet with
# one wrong length field, not random noise that every decoder rejects at the first byte.
#
# Committed, small, and derived: a few hundred packets rather than a trading day, chosen by walking the file and
# taking every Nth so the sample spans the session rather than its first second.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
capture="${1:-}"
out="${here}/fuzz/corpus"

if [[ -z "${capture}" || ! -f "${capture}" ]]; then
  echo "usage: fuzz-corpus.sh <capture.pcap>" >&2
  echo "  get one with: curl -s 'https://iextrading.com/api/1.0/hist?date=20170826'" >&2
  exit 1
fi

python3 "${here}/scripts/extract-corpus.py" "${capture}" "${out}"
echo "fuzz-corpus: $(find "${out}" -type f | wc -l | tr -d ' ') files under fuzz/corpus"

#!/usr/bin/env bash
# Builds a fuzzing corpus, from a real capture where one exists and from the library's own encoders where
# one does not.
#
# A corpus of hand-written packets fuzzes the cases somebody imagined. A corpus of real ones starts every
# mutation from a packet that actually parses, which is where the interesting failures are: a valid packet with
# one wrong length field, not random noise that every decoder rejects at the first byte.
#
# iextp/deep/moldudp64 come from a real IEX HIST capture and need one passed in. ouch/soupbintcp do not: OUCH is
# a private client-exchange order-entry session, not multicast market data, and no free public capture of one
# exists. tools/corpus_gen drives several real venue::order_session flows and writes every raw byte crossing
# the wire, so those seeds are real encoder output rather than a second, hand-written approximation of the wire
# layout that could disagree with the library and be wrong in a way nothing catches.
#
# Committed, small, and derived: a few hundred packets rather than a trading day, chosen by walking the file and
# taking every Nth so the sample spans the session rather than its first second.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
capture="${1:-}"
out="${here}/fuzz/corpus"

if [[ -n "${capture}" ]]; then
  if [[ ! -f "${capture}" ]]; then
    echo "fuzz-corpus: ${capture} does not exist" >&2
    exit 1
  fi
  python3 "${here}/scripts/extract-corpus.py" "${capture}" "${out}"
else
  echo "fuzz-corpus: no capture given, skipping the iextp/deep/moldudp64 extraction"
  echo "  get one with: curl -s 'https://iextrading.com/api/1.0/hist?date=20170826'"
fi

if [[ ! -x "${here}/build/dev/tools/corpus_gen" ]]; then
  cmake --preset dev >/dev/null
  cmake --build --preset dev -j 8 --target dfr_corpus_gen >/dev/null
fi
"${here}/build/dev/tools/corpus_gen" "${out}"

echo "fuzz-corpus: $(find "${out}" -type f | wc -l | tr -d ' ') files under fuzz/corpus"

#!/usr/bin/env bash
# Re-fetches the committed capture from IEX's own historical data API, in case it is ever needed again.
#
# Not run by CI or by any other script: captures/20170826-iex-deep.pcap.gz is already committed, and IEX's
# historical files are static, so this exists for provenance and for the day the file needs replacing rather
# than for routine use.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
date="20170826"
out="${here}/captures/${date}-iex-deep.pcap.gz"

link="$(curl -s "https://iextrading.com/api/1.0/hist?date=${date}" \
  | python3 -c "import json,sys; print(next(e['link'] for e in json.load(sys.stdin) if e['feed']=='DEEP'))")"

tmp="$(mktemp)"
curl -sL "${link}" -o "${tmp}"
gunzip -t "${tmp}"

# Re-compressed at level 9 rather than saved as fetched, so a re-run reproduces the exact committed bytes
# regardless of the compression level whatever served the original happened to use.
gunzip -c "${tmp}" | gzip -9 > "${out}"
rm -f "${tmp}"

echo "download-capture: wrote ${out} ($(wc -c < "${out}" | tr -d ' ') bytes)"

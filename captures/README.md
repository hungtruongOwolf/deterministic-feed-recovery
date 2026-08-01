# Real capture, committed

`20170826-iex-deep.pcap.gz` is IEX's own published historical sample: DEEP 1.0 over IEX-TP, 2017-08-26, 20,145
packets, 48,635 messages. Free, no registration, fetched from `iextrading.com/api/1.0/hist`, the same file
`docs/BENCHMARKS.md` and the DEEP field-offset verification in the main README were checked against.

Committed gzipped (709 KB, 3.4 MB raw) so `tools/verify` (which needs a real capture, not a synthetic
stream, to answer "does the recovery pipeline hold on a real feed's actual message-size distribution, its
heartbeats, its quiet periods and its bursts") has one to run against in CI rather than only by hand on
whichever machine happens to have downloaded it. Before this, that question was answered locally and never
checked again.

To refresh it (IEX's historical files do not change, so this should only ever be needed if the file becomes
unavailable at the same URL):

```sh
./scripts/download-capture.sh
```

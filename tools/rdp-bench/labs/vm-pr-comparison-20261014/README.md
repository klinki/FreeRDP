# Frozen VM comparison lab (2026-10-14)

This is the retained Mac/Docker/Windows ARM lab coordinator, build/deployment code, image observer, reporter and regression fixtures. It uses the existing `tools/rdp-bench/upstream-method` helpers and `upstream_method.py`. The full transport benchmark suite remains the entry point documented in `tools/rdp-bench/README.md`.

The comparison measured production X11 clients from local commit `79ee244a4d5b2d5a280e165be4c643478484bf9e` and PR snapshot `a306a67c198f4493d8d2000bc10efd75b2432020`. It is eight fresh connections, one per client and 0/25/50/70% loss, with a 45-second warmup and up to 120 seconds measured. Both 70% connections terminated. These are unseeded exploratory graphics trials, with no bandwidth cap and no verified goodput or latency-tail measurements.

`run-comparison.measured-20261014.py` is the exact coordinator used for both campaigns (SHA-256 `b7ff06a819f9cbe719528c4cca9d62d0c9355f7e0c6aea70fd97063356cfc936`). `run-comparison.py` adds the subsequently tested capture-process guard. It defaults to **no packet capture**; `--pcap` enables optional captures. Neither native client nor measured data was changed by the guard fix. The original smoke producer's singular-footer parsing failure is retained beside the corrected offline validation.

These are dated lab scripts with explicit Mac workspace, VM address, owned container name, mount paths, SSH socket and pinned source revisions. To inspect or run the offline fixtures, leave the files together and run:

```sh
python3 test-process-guard.py
python3 test-image-progress.py
python3 test-report-comparison.py
```

For reproducing this particular lab, restore the scripts and evidence to `/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014`, with sources from `git archive` of the exact revisions in `local-source` and `build/pr13608-reference-20261013/original-source`. Existing receipts deliberately block overwriting past setup/trials. A new campaign requires a new reviewed workspace/ownership identity, matching mount paths and a fresh authenticated SSH socket; do not blindly run deployment or setup scripts from an extracted results directory. `prepare-linux.py` records the dependency inventory and source/binary hashes. `prepare-windows.py` checks the VM identity and certificate and hash-verifies the three workload/collector scripts. Start private Xvfb at 1280x800x24, establish the owned veth adaptation before RDP authentication, then invoke `run-comparison.py` in a PTY with a fresh output directory. Its password prompt sends credentials only through client stdin. Preserve output even when the coordinator exits nonzero because a session failed.

The reporter takes `--index`, `--output-prefix`, `--capture-mode disabled|requested`, and mandatory `--campaign-complete`. Analysis verifies a closed campaign, source/build provenance, capture configuration, actual RDP/PDH instance and conservative clock-bounded counter intervals. Restore the recorded directory layout before regenerating; frozen JSON contains original absolute paths.

The published-method inputs are pinned to OpenThinRDP `142434adf715c3f3352c0e6dd7b7ece90244aeec`. Windows-downlink loss affects TCP and UDP; packets >=512 bytes also get 1 ms delay and 70% reordering. Zero loss retains those large-packet treatments. Veth replaces unavailable IFB, ping verification was omitted, and independent connections replace the upstream sequential ramp. See the result note for interpretation and limitations.

All owned Windows tasks/processes were cleaned up; guest evidence files remain. The owned Linux network was restored, container removed, SSH master closed and temporary power assertion ended. Cleanup receipts are bundled. Full exploratory packet captures and compiled build trees remain in the original local build directory and are excluded from the ZIP; omitted files are named and hashed in `VM-PACKAGE-MANIFEST.json`. No raw data from the capture-confounded campaign is pooled with the no-capture campaign.

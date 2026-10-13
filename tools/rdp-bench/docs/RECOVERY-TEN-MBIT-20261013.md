# Transport rerun after the UDP2 receipt-window repair

This repeats the loaded-upload conditions from the candidate main campaign in
[LOSS-STALLS-20261012.md](LOSS-STALLS-20261012.md), using the receipt-window fix
from `189cc1b88`. The checkout at campaign start was `c6e86d5a4`; those commits
have identical native transport sources. The native adapter binary is
`ae0430db98b053196067ea9a287b1ac6a7032e98b6669889baecd877efd10a9f`.

The exact old fixed peer remains
`b4c64d6d1342e65fc7402d57aa2cecb8c62e2256f0324bdca9a67ee6103c2bfd`,
with its adjacent old WinPR, 128 KiB queue and timer recovery. The current DUT
uses its own frozen WinPR and fast recovery. The native sources, harness,
libraries, settings and source hashes were frozen in
`build/udp-recovery-ten-mbit-20261013` before measurement.

## Matrix

| Profile | Rate each direction | Delay each direction | Jitter each direction | Random loss each direction |
|---|---:|---:|---:|---:|
| `ten_mbit_clean` | 10 Mbit/s | 15 ms | 0 | 0 |
| `ten_mbit` | 10 Mbit/s | 15 ms | ±1 ms | 0.05% |

Each profile has a 30 ms queue, with no extra duplication, reordering or
ACK-delay treatment. Shaping counts UDP datagram bytes plus 28 bytes of
IPv4/UDP overhead. It excludes Ethernet overhead.

The complete main matrix is **two profiles × two DUT queue caps × five seeds**,
twenty candidate trials. Caps are 48 and 64 KiB; seeds are
11, 29, 47, 71 and 101. Every condition uses 5 seconds warmup, 30 seconds
measurement, 10 seconds drain, 16 KiB application records, loaded upload and
20 Hz scheduled probes. Variant order is shuffled deterministically per seed.
Both endpoints advertise LogWindow 5 with a 31-packet effective window.

Four separate 1/3/3-second preflights use seed 11. All four verified payload
integrity and queue accounting; clean 64 KiB had a relay p99 of 1.518 ms,
above its 1.500 ms budget, and remains timing-invalid. These short diagnostics
are not pooled into the main results. Main trials were run once in the planned
matrix; failed trials were retained.

Loss tracing is off, including the recovery-trace environment variables.
Counters, queue observations and ordinary benchmark event observations are on.
No builds or tests competed with the serial timed campaign.

## Measurement and comparison

Verified goodput counts measured application payload with complete byte,
hash, CRC-record and order checks after drain. Incomplete integrity never
produces verified-goodput values. Timing-invalid results remain diagnostic.

Probe RTT starts before native queue admission and ends at the returned
transport response. Scheduled RTT also includes lateness relative to the
intended injection deadline. p95/p99 values are medians of per-trial
percentiles, never pooled percentiles. Observed-response percentiles exclude
missing probes; missed injections, timeouts and native failures remain beside
them. Probe counts include warmup, while percentiles use measurement probes.

This is raw native reliable UDP2 over local sockets and the Python relay.
The Windows incoming-channel recovery evidence remains in
[UDP-RECOVERY-20261013.md](UDP-RECOVERY-20261013.md). The present measurements
describe loaded upload and transport probe RTT, without a physical desktop
latency claim.

The comparison is historical and descriptive. Queue caps, profiles, seeds,
workload, durations and peer match the old main candidate. Native ACK/RTO,
channel retention, receipt tracking, harness/deadline handling and the DUT's
WinPR have changed since that campaign. Source hashes therefore do not define
an identical strict comparison group. This does not isolate the last receipt
repair from the intervening changes.

Qualified comparisons use the same eligible seeds on both sides. Complete
cohort counts, failed/invalid trials and all-observed diagnostics are also
retained, rather than comparing different seed populations silently. The
portable lab has no accepted calibration/headroom receipt, so results remain
provisional even where per-trial timing and payload integrity pass.

[JSON](../results/recovery-ten-mbit-20261013.json),
[CSV](../results/recovery-ten-mbit-20261013.csv) and
[readable results](../results/recovery-ten-mbit-20261013.md) preserve the full
comparison and failures. Raw sealed trials and the executable frozen harness
remain in the build directory above.

## Results

The table uses the same eligible seeds on both sides; values are old main
candidate → current candidate. These are medians of per-trial statistics.

| Profile / queue | Matched N | Verified goodput Mbit/s | Probe RTT p95 ms | Probe RTT p99 ms |
|---|---:|---:|---:|---:|
| Clean / 48 KiB | 5 | 8.208 → 8.206 | 48.280 → 49.119 | 49.928 → 52.805 |
| Clean / 64 KiB | 4 | 8.799 → 8.793 | 74.932 → 75.695 | 76.332 → 80.908 |
| Impaired / 48 KiB | 5 | 8.213 → 8.134 | 64.030 → 64.543 | 87.310 → 90.804 |
| Impaired / 64 KiB | 5 | 8.497 → 8.500 | 80.034 → 80.331 | 104.588 → 116.733 |

All twenty current trials verified complete payload streams. Nineteen were
timing-qualified COMPLETE. Clean 64 KiB seed 11 was a probe-service FAIL with
87 missed injections, zero response timeouts and complete payload integrity.
Its relay p99 lateness was 59.869 ms C→S and 47.435 ms S→C, against a 1.500 ms
budget. Those whole-trial observations confound performance attribution;
untraced results do not establish the cause of each missed injection. The
trial is retained diagnostically and excluded from the qualified comparison.
Its corresponding old seed is also excluded from the matched row above.

The current campaign scheduled 14,000 probes, injected 13,913 and observed
all 13,913 responses. The old candidate scheduled/injected/observed all
14,000. There were no native terminal or benchmark-deadline failures in the
current main campaign. The four preflights remain separate, with their one
timing-invalid result retained.

Goodput changed by less than 1% in every matched row. The observed p99 values
rose by about 2.9 / 4.6 / 3.5 / 12.1 ms respectively. These small historical
cohorts provide no isolated causal estimate for the receipt-window repair.
The 48 KiB configuration retains the lower loaded probe latency; the Windows
campaign separately establishes recovery across its exercised high-loss
intervals.

## Reproduce with the frozen inputs

Use a new output directory; retain previous attempts. The frozen harness,
libraries, configs and comparison generator are also included with the raw
receipts in the Downloads suite.

```sh
python3 -B build/udp-recovery-ten-mbit-20261013/source/tools/rdp-bench/bench.py type1 \
  --suite build/udp-recovery-ten-mbit-20261013/suite.json \
  --variants build/udp-recovery-ten-mbit-20261013/variants.json \
  --profiles build/udp-recovery-ten-mbit-20261013/profiles.json \
  --peer-library build/udp-recovery-ten-mbit-20261013/binaries/peer/libfreerdp3.dylib \
  --peer-env build/udp-recovery-ten-mbit-20261013/peer-env.json \
  --output build/your-new-recovery-run
```

The raw configs record absolute frozen library paths for this checkout. When
moving them to another directory, update those paths and the DUT's frozen
WinPR search path together, retain hashes, and use a distinct output root.

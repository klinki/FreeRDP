# Protocol-specific VM loss lab

This dated lab compares production Linux X11 clients from the local `79ee244`
checkout and explicitly identified upstream PR builds against the existing
Windows VM. Paths, account, certificate pin, source mounts and authenticated SSH
socket are lab configuration. Passwords are read once with `getpass` and supplied
only to the clients' stdin; they are never configuration or receipt fields.

Each production phase has sixteen fresh transport connections:

| Selected ingress protocol | Loss | Clients | Warmup | Measurement |
|---|---|---|---|---|
| TCP | 0%, 25%, 50%, 70% | local, PR | 45 s | 120 s |
| UDP | 0%, 25%, 50%, 70% | local, PR | 45 s | 120 s |

At each loss level the client order alternates local/PR, PR/local. TCP conditions
precede UDP conditions. There is one unseeded connection per condition; all
outcomes remain in the index. A lifecycle/ownership failure stops the phase and
retains unreached rows. Native client failures are retained and the next planned
condition proceeds after verified cleanup. Existing Windows desktop/login state
may be reused by fresh RDP transport connections.

The impairment affects only selected IPv4 traffic arriving from the Windows VM.
“TCP-only” and “UDP-only” describe the injected loss: both RDP transports stay
enabled. This is not a forced-TCP or forced-UDP client mode.
TCP-only and UDP-only describe the impairment selector: both RDP transports
remain enabled in both clients.
Selected IP packets of at least 512 bytes also receive 1 ms delay and 70%
reordering; selected smaller packets receive loss alone. The other protocol uses
the explicit bypass band. There is no bandwidth cap and return traffic is clean.
The owned veth adaptation replaces unavailable IFB. A selector change requires
checked cleanup and a new setup between closed protocol groups. All qdisc
counters, including bypass drops/backlog, are retained.

Baseline, fixed-PR, and post-fix all-IPv4 phases have independent output folders,
source declarations and fingerprints. They are never pooled. The all-IPv4 phase
has eight connections, using the same four loss levels and two clients.

Example production invocation, after root-owned VM/container/source preparation:

```sh
python3 run-protocol-loss.py --settings settings.json --phase baseline \
  --protocols tcp udp --output campaign-baseline
```

`settings.json` identifies `binary`, `source_root`, `build_root`, `source_files`,
declared source `commit`, descriptive `label`, and optional `build_receipt` for
each variant. Source files can be full repository-relative paths or core
filenames. The phase's `frozen-variants.json` records actual binary, library and
source hashes. A declared base commit alone does not identify a staged trace or
fix overlay; preserve the hashes and specific build receipt as well.

`graceful_collector_stop=true` requires the newly deployed collector. Its optional
`StopFile` parameter defaults empty for older callers. The host controller writes
an owned stop request, waits up to 15 seconds for a flushed `collector_end`, and
records any need for receipt-guarded fallback cleanup. Each arrived counter set
or provider error is retained before the stop sentinel disposes the query. The
baseline never contributes cooked rates. Only fully contained, valid ten-second
nominal intervals from the corroborated Windows session and recorded clock
bounds contribute metrics. Raw outside-phase provider errors remain separate.

Tracing and packet capture are disabled in production phases. Separate PR70
diagnostics can use either UDP-only or all-IPv4 impairment:

```sh
python3 run-protocol-loss.py --settings settings-diagnostic.json \
  --phase baseline-pr70-udp-diagnostic --protocols udp --losses 70 --variants pr \
  --trace --pcap --capture-max-packets 1000000 --output diagnostic-pr70-udp
```

The trace is armed five seconds before loss, with a one-million-event tail ring.
The native metadata/footer describe overwritten prefix and retained coverage.
The two UDP-header captures use 256-byte snap length and a hard packet-count
bound: each file is at most `24 + 272 * capture_max_packets` bytes. They do not
rotate or overwrite earlier packets. Reaching the count limit or recording
kernel drops invalidates full-phase wire coverage; partial evidence remains.
Capture cleanup uses owned PIDFD/birth-time/argv guards. Extra observers make
the entire diagnostic dataset ineligible for performance comparison. An outer
bounded TCP socket observer, when present, is separate diagnostic evidence.

`run-protocol-loss.buffered-diagnostic.py` is a separate derivative of the frozen
coordinator. It permits only a traced, captured PR70 UDP diagnostic and removes
tcpdump's `-U` per-packet file flushing. Normal buffered output, the same 256-byte
snap length, hard packet-count bound, final statistics footer and ownership
guards remain. `capture_flush_mode=buffered` is recorded in campaign,
run-configuration, trial and provenance receipts. It does not change the
production runner or native recovery policy. Buffering can still influence the
diagnostic; a healthy traced run never replaces a failed performance trial.
The buffered variant keeps both captures running through client teardown and
the native trace dump. The first two diagnostics closed captures before that
tail, so native repairs after capture closure are explicitly outside wire
coverage and cannot establish recovery under the requested loss profile.

The fixed production client uses native commit `49fcbb72c`. Only v3 stops using
the legacy 16-second ordered-progress watchdog. The existing v3 peer-silence
deadline, own retransmission limits/RTO, ACK/AoA policy and receive bounds remain.
A genuinely unrepaired channel gap can remain while the peer is active; the
visual-progress report therefore qualifies connection survival separately.
The new native live-peer regression is RED with the old policy and GREEN with
the fix, releasing retained `b` after late `a` at 17.5 seconds. Separate tests
cover 16.5-second peer silence and out-of-range negative acknowledgements.
The buffered derivative records
`capture_stop_order=after-client-stop-and-native-trace-dump`; captures stay open
through client shutdown and the native dump, then close before host child waits.
Earlier diagnostics retain their original shorter capture coverage.

Run post-campaign analysis only after the coordinator's `finished_utc` exists:

```sh
python3 report-protocol-loss.py --index campaign-baseline/trial-index.json \
  --output-prefix reports/baseline --campaign-complete
```

Reports preserve phase/protocol/client/loss keys, every planned condition,
source identities, actual survival, UDP versus main-TCP errors, raw/admitted
provider events, realized selected and bypass drops, and measured visual
prefixes. Windows output FPS means frames made available by the server.
Low-frequency central pixel changes do not establish physical client FPS,
CRC-verified goodput, or transport/input-to-display p95/p99; those fields remain
unknown. Single-connection results are exploratory and carry no significance
claim. Source-only reporter fixtures run between timed campaigns, never during
measurement.

After all three production phases close, `compare-protocol-phases.py` accepts
their phase reports through `--baseline`, `--fixed`, and `--fixed-all`. An optional
`--prior-all` references the earlier no-PCAP all-IPv4 campaign with its different
collector/cleanup harness explicitly retained. Repeated `--diagnostic-report`
options retain observer runs separately; they never enter production metrics.
The output prefix `reports/protocol-loss-before-after-20261014` produces JSON,
a 40-row production-trial CSV, a separate `-pairs.csv` (16 protocol before/after
pairs plus eight historical all-IPv4 pairs when provided), and Markdown.

The comparison adds the fraction of linked 1 Hz pixel samples that changed and
the longest consecutive unchanged sampled span before the first logged terminal
event. Zero changed comparisons or a span of at least ten seconds flags a
surviving session for inspection. This diagnostic threshold does not declare a
transport failure or prove that no activity occurred between captures. Survival,
partial FPS with its admitted sample count, and sampled visual progress must be
considered together.

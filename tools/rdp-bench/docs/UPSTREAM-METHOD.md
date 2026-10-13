# OpenThinRDP loss-ramp method

This adapter records the method in [OpenThinRDP `tests/udp-bench`](https://github.com/OpenThinRdp/openthinrdp/tree/main/tests/udp-bench), preserves the exact downloaded source hashes, and summarizes raw per-instance Windows RemoteFX counters. It does not launch a VM, shape traffic, or substitute client process survival for successful graphics delivery.

The upstream session uses Linux Xvfb at **1280×800**, progressive GFX with AVC420/AVC444 disabled, `/network:auto`, and a 20-second connection timeout. The supplied `load.ps1` displays full-screen coloured noise at a nominal 33 ms timer interval. The ramp applies loss to **all IPv4 traffic arriving from the Windows host**, including TCP and UDP, using Linux ingress `tc` and IFB. IP packets of 512 bytes or more also receive 1 ms delay and 70% reordering; smaller packets receive loss alone. There is no 10 Mbit/s cap. Each line setup verifies small and large pings before the 120-second survival interval starts. The ramp stops at the first client process termination.

The requested **25%, 50%, 70%** levels replace upstream's default 50%, 70%, 80%. A Mac UDP-only proxy remains a distinct adaptation: dropping bytes on a forwarded TCP stream does not emulate TCP packet loss and must never be labelled as this method.

Freeze the exact inputs before running:

```sh
python3 tools/rdp-bench/upstream_method.py plan \
  --source-dir build/pr13608-method/upstream \
  --loss 25 50 70 --seconds 120 \
  --output build/pr13608-method/method-manifest.json
```

Use the fetched upstream session, line, ramp and load scripts. The source scripts are not vendored into this checkout. Record their repository commit, hashes, candidate binary hash and effective arguments, OS/kernel version, network interface/source-IP filter and qdisc snapshots in the campaign provenance. Run only in an isolated benchmark network; the upstream line script replaces ingress qdiscs on its chosen interface.

For interactive load launch, `upstream-method/Invoke-Load.ps1` creates one unique `RdpBench-Pr13608-*` task for the authenticated Windows user's existing interactive session. It refuses to overwrite a task and applies a bounded execution time. Pass the downloaded upstream `load.ps1`; record the emitted task/hash receipt. During cleanup, stop and unregister exactly that task, and terminate only an owned, recorded workload PID if necessary. Do not stop unrelated PowerShell or scheduled tasks.

The original `counters.ps1` can run unchanged for human-readable upstream output. Run the supplemental original collector on the same Windows host for machine-readable evidence:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Collect-Counters.ps1 `
  -OutFile C:\rdp-bench\upstream-counters.jsonl -Seconds 600 -Interval 10
```

The collector maps English counter names to localized Perflib IDs, retains available provider paths and discovery errors, and records all session instances. One bounded continuous PDH query produces a baseline followed by samples at the requested ten-second cadence. The baseline remains in the evidence and never contributes a measured rate; every later set covers the interval since the previous PDH timestamp. There is no paired-query restart or additional sleep. Actual interval lengths, collector source hash, provider error events and final cadence statistics are recorded. A lost provider does not discard a preceding valid sample. Missing providers, nonzero PDH status and invalid numeric values remain unknown. On Windows editions without a RemoteFX provider, the run is still retained with unavailable metrics. Provider availability is checked, not assumed.

Identify the benchmark session's actual provider instance before aggregation, then pass it as `-Instance` to the collector or `counter_instance` in the campaign. Instance ambiguity is rejected rather than choosing the first RDP session. The client name alone does not prove which provider instance belongs to it.

Campaign JSON records every planned step, including steps not reached after termination:

```json
{
  "method_fidelity": "exact Linux ingress IFB/netem; requested loss levels",
  "clock_alignment": {"method": "record actual evidence here", "uncertainty_ms": null},
  "counter_instance": "the verified benchmark provider instance",
  "provenance": {"binary_sha256": "actual hash", "source_commit": "actual source revision"},
  "steps": [
    {"id": "loss25", "loss_percent": 25, "requested_s": 120,
     "start_utc": "2026-10-13T12:00:00Z", "end_utc": "2026-10-13T12:02:00Z",
     "client_alive_end": true, "outcome": "COMPLETE", "tunnel_status": "recorded evidence",
     "network": {"qdisc_before": {}, "qdisc_after": {}, "ping_verification": {}}},
    {"id": "loss50", "loss_percent": 50, "requested_s": 120, "outcome": "NOT_RUN"},
    {"id": "loss70", "loss_percent": 70, "requested_s": 120, "outcome": "NOT_RUN"}
  ]
}
```

When host clocks differ, use `windows_start_utc`/`windows_end_utc` translated from recorded clock-alignment measurements. Counter intervals crossing a step start are excluded. Without alignment evidence, phase assignment is provisional. Capture UDP tunnel ready/failure and TCP fallback evidence separately from the client PID checks, and retain client logs/exit code and realized network/qdisc counters per step.

```sh
python3 tools/rdp-bench/upstream_method.py report \
  --campaign build/pr13608-method/campaign.json \
  --counters build/pr13608-method/upstream-counters.jsonl \
  --output build/pr13608-method/report
```

The JSON/CSV report retains all steps and missing/error states. Server output FPS is frames produced by the server, not physically displayed client FPS. Network bandwidth and server send-rate counters do not establish payload integrity or verified application goodput; those claims remain unavailable in this method. The current Windows collector's live execution and exact-method outcomes must be recorded with each campaign; Python fixture regressions alone do not certify Windows provider behaviour.

## Linux veth adaptation when IFB is unavailable

Docker Desktop's Linux kernel in this lab does not provide IFB. The original IFB recipe cannot run unchanged. `upstream-method/linux-ingress.py` instead redirects matching ingress IPv4 packets into the egress qdisc of an owned veth endpoint, then receives them through its peer. The receive endpoint has the ordinary interface's MAC and no IPv4 address, so the ordinary interface retains the local IP address. This preserves packet-level TCP and UDP loss in the server-to-client direction, using the same IP-length classifier and netem delay/reorder/loss settings. It is explicitly labelled a **veth adaptation**, rather than an unchanged upstream execution.

Run only in the dedicated container/network namespace with `NET_ADMIN`, without a host network namespace. The adapter refuses existing veth names and existing ingress/clsact qdiscs. It records its namespace and interface indices, adds ownership aliases to both endpoints, and checks the recorded indices, MAC identity and absence of receive-side IPv4 before later mutations. It temporarily disables reverse-path filtering on the namespace's `all`, ordinary interface and new receive endpoint; the original persistent interfaces' values are saved and restored. The receive endpoint disappears on cleanup. MTU is set to 1500 on the ordinary interface and both veth endpoints; the ordinary interface's original MTU is restored. In this Docker lab the initial interface MTU was 65535, so initial MSS behaviour differs unless that adjustment precedes the connection.

```sh
python3 linux-ingress.py --state /tmp/pr13608-line.json --output /tmp/setup.json \
  setup --vm-ip 192.168.64.2 --interface eth0
python3 linux-ingress.py --state /tmp/pr13608-line.json --output /tmp/loss25-start.json \
  change --loss 25
# Run the requested 120-second step, checking the exact owned client PID.
python3 linux-ingress.py --state /tmp/pr13608-line.json --output /tmp/loss25-end.json snapshot
# Repeat change/snapshot for 50% and 70%, unless the session has ended.
python3 linux-ingress.py --state /tmp/pr13608-line.json --output /tmp/cleanup.json off
```

Each `change` replaces the two child netem qdiscs, resetting their counters and discarding queued packets, as upstream's complete line replacement does. Save the previous end snapshot before changing a level. Changes verify the owned ingress redirect, priority root, classifier identities and child handles before deleting qdiscs. Cleanup verifies the ingress handle/kind and source redirect target before removing the ingress qdisc. Commands use argument arrays and fail immediately on nonzero exit. Receipts include interface addresses/MTUs, source filter, classification rules, full qdisc counters/configuration, and per-band transmitted/dropped/backlogged packet counts. The ratio `drops / (transmitted + drops)` describes completed qdisc attempts; pending packets are separate, and qdisc drops are not automatically classified as random loss. Sum only the two child bands, not their parent too.

The original script's 100-small/100-large ICMP verification is unavailable on this Docker-to-UTM route: no ping replies were obtained with a clean line. Retain that failure as a method deviation. A clean TCP/TLS connection, a deliberately blocked connection at 100% loss, and actual per-phase source-filter/netem counters can prove the adaptation affects VM traffic, but they do not substitute for the original two ICMP loss measurements. Record the proof results and exact protocol used. Do not label a run fully equivalent to the upstream IFB/ping method.

Cleanup requires the recorded namespace and both ownership aliases, removes only the owned ingress qdisc/veth pair, and restores recorded interface settings. If setup fails partway, its state is retained for inspection. Automated cleanup is available only once the recorded interface identities and any installed ingress redirect can be verified; incomplete setup can deliberately refuse cleanup. Inspect the exact recorded objects before manual removal. Legacy state files without interface-index receipts are refused; use the matching original helper for cleanup before creating a fresh state. Do not delete unrecognized interfaces or qdiscs to force a retry.

## Fresh-session trials after a terminated ramp

Preserve the original sequential ramp and its stop-on-first-exit rule. If that ramp terminates at 25%, explicitly mark 50% and 70% `NOT_RUN` in that campaign. A request to measure every level can then use **one separate, fresh session per remaining level**, each for 120 seconds or until termination. Label those as fresh-session diagnostics, retain every attempt, and do not restart failed measurements to select survivors. A fresh 25% trial is an independent repeat, not a replacement for the failed original ramp. Keep the same frozen client/library hashes, codec settings, load source and network method across these sessions.

`upstream-method/host-control.py` provides per-trial Windows control through an **already authenticated SSH control socket**. It handles no passwords, Linux client launch, Docker setup or shaping. Deploy the pinned upstream `load.ps1`, `Collect-Counters.ps1` and new `Invoke-Workload.ps1` into a shared scripts directory first. The existing `Invoke-Load.ps1` remains available for its previous callers. Use a unique Windows output directory, local output directory and task name for every session:

```sh
python3 tools/rdp-bench/upstream-method/host-control.py \
  --ssh-target david@192.168.64.2 \
  --ssh-socket /private/tmp/freerdp-pr13608-vm.sock \
  --scripts-root 'C:\RdpBenchRecv20261013' \
  --windows-root 'C:\RdpBenchRecv20261013\loss50' \
  --task-name RdpBench-Pr13608-loss50-unique \
  --output build/pr13608-method/fresh-loss50 prepare
```

Use the same global arguments for subsequent commands, in this order:

1. Start the owned Linux client with credentials on stdin, certificate pinning and unique client logs. Record its actual container PID and `/proc` start ticks. Wait for authenticated RDP/UDP setup.
2. `discover` retains raw `quser` and provider paths, requires one active RDP session for the authenticated Windows user, and correlates that session name with the actual PDH instance. Missing PDH instances remain unavailable.
3. `clock` records three Windows-versus-host UTC bounds. Record Linux-versus-host bounds separately. Repeat clock measurements after each trial to bound drift.
4. `load --seconds 180` creates exactly one unique interactive workload task and waits for its actual PID receipt. The wrapper invokes the original upstream load in the same PowerShell process and records PID, creation UTC, RDP session ID, source/wrapper hashes and termination when available. Confirm the workload is active before applying loss.
5. Run `collector --seconds 180 --interval 10` as a separately owned **foreground** host process while the trial runs. It records the actual collector PID, creation UTC, session ID and trial owner before sampling. Root owns that SSH invocation's lifetime; no detached daemon is created. Use a longer bounded duration if setup/warmup would otherwise exceed 180 seconds.
6. Run the Linux ramp runner with just that level (`--loss 50 --seconds 120`) and retain its unique campaign/network receipts.
7. `cleanup` validates the exact task action and stops only receipt-owned PIDs whose creation time and session still match. It does not select all processes by script path. `fetch` copies the allowlisted trial artifacts and records their SHA-256 hashes. Fetch after collector cleanup for a complete JSONL file. Windows trial files remain preserved.

A trial that terminates before 10 seconds cannot provide a fully-contained 10-second primary counter interval. Its loss-phase server FPS remains unavailable. Do not substitute a preceding clean or cross-boundary sample. An optional separate one-second collector is diagnostic and must use separate outputs/labels; never merge its samples into the upstream ten-second series.

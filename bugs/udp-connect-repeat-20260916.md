# Ten UDP connection checks — 2026-09-16

Outcome: **10/10 connections established UDP, migrated the receive path, and
received remote graphics. Zero observed transport failures or unexpected
disconnects during the observation windows.** The earlier UDP timeout was not
reproduced; this sample does not establish its cause or rule out an intermittent
bug.

## Setup and method

- Local fixed SDL client at commit `3b6f9b67bae7c336e71b6be932d98a633f2b0b81`.
- VM `192.168.64.2`, existing David account; no relay, packet loss injection, or
  transport/source changes.
- Original external display pair: M27UP 4K/175%, Dell FHD/100%; monitor IDs 3,2.
- `/gfx:AVC444:on /network:lan /multimon +f +multitransport`, same session settings
  as the drag investigation. Automatic reconnect was not enabled.
- Ten separate client processes, sequentially, approximately 25 seconds each,
  with 3 seconds between attempts. Each stayed connected for at least 18.86
  seconds after both UDP receive migration and the first graphics frame.
- A PTY made the logs immediately readable. Times below are elapsed seconds from
  process launch to observing the log marker, not packet-level timing.
- The harness recorded UDP handshake, TLS, tunnel establishment, migration,
  graphics EndFrame PDUs, process liveness, and transport errors. Intentional
  SIGINT disconnect/cleanup errors were excluded from failure classification.
- Computer Use showed the remote desktop in spot checks. This is primarily a
  connection/received-graphics test, not a sustained load test or a complete UI
  responsiveness test on all ten attempts.

## Results

| Attempt | UDP tunnel ready | UDP receive migrated | First graphics frame | Frames received | Transport faults |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 1.362 s | 5.168 s | 5.822 s | 16 | 0 |
| 2 | 1.268 s | 1.269 s | 2.310 s | 16 | 0 |
| 3 | 1.314 s | 1.315 s | 2.219 s | 12 | 0 |
| 4 | 1.773 s | 5.107 s | 5.528 s | 16 | 0 |
| 5 | 1.071 s | 1.072 s | 6.140 s | 13 | 0 |
| 6 | 1.543 s | 5.091 s | 5.582 s | 15 | 0 |
| 7 | 1.072 s | 1.075 s | 1.704 s | 43 | 0 |
| 8 | 1.502 s | 5.066 s | 5.546 s | 16 | 0 |
| 9 | 1.313 s | 1.315 s | 2.249 s | 18 | 0 |
| 10 | 1.259 s | 1.260 s | 2.232 s | 16 | 0 |

All ten client processes were alive immediately before the intentional
disconnect. Every attempt printed the initial “staying on TCP to preserve
ordering” warning **and subsequently logged UDP receive migration**. Median
receive migration time was 1.315 seconds. Four attempts had a several-second
gap between tunnel establishment and receive migration; the current evidence
does not identify which side caused that delay.

## Meaning of the TCP warning

`multitransport_udp_connect_thread` initially leaves the DVC send/receive
migration flags false when the UDP tunnel finishes after ACTIVE without
Soft-Sync. Later, `multitransport_check_fds` sets the receive-migrated flag when
it observes the first valid UDP DVC PDU. Thus the initial warning describes a
startup state; it does not prove the session remains TCP-only. Send and receive
migration are separate. These measurements prove tunnel establishment and UDP
receive migration, not that every RDP channel or both directions use UDP.

The first UDP-enabled session in the earlier drag investigation also logged
receive migration after the warning. Calling it a persistent TCP fallback was
incorrect. A later attempt at approximately 08:20 did have genuine
`RDPEUDP2 send timeout`, `Connection reset by peer`, and
`Tunnel Create Request send failed` messages. That remains a separate,
unexplained failure, not reproduced here.

## Runner qualification and evidence

The original runner also required an ACTIVE debug line, but its selected log
filters did not enable the `com.freerdp.core.rdp` logger. It therefore produced
provisional `failed_or_incomplete` labels and ran every attempt for the full
25-second startup window. These labels are **not transport failures**.

Final classification was checked against the actual recorded tunnel/migration
markers, graphics frames, process liveness, pre-disconnect faults, and at least
12 seconds after readiness. All ten meet those criteria. The original
observations and exact harness used are retained unchanged for audit. The
reusable harness now enables the RDP logger and uses received graphics rather
than that optional debug marker as its readiness criterion.

- Reusable harness: `tools/udp-review-tests/integration/udp-connect-repeat.py`.
  Password is supplied through `RDP_TEST_PASSWORD`, not stored in the script.
- Evidence: `tools/udp-review-tests/logs/udp-connect-20260916/` contains original
  observations, final analysis, the original harness, and per-attempt extracts
  of transport/graphics markers.
- Raw local logs: `/tmp/rdp-udp-connect-20260916/attempt-01.log` through
  `attempt-10.log`. No TLS secrets were enabled. Raw debug logs were kept local.
- After the measured ten attempts, the normal fixed session was restored in a
  separate connection; it is not included in the statistics above.

No FreeRDP transport implementation was changed during this investigation.

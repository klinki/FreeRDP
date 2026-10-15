# Session, input, and display qualification — 2026-10-15

The two auto-reconnect cases passed in standard and launcher-bridge modes with the
same client process across both connections. A loopback-only relay forwards the
original encrypted bytes and drops its current socket pair when Microsoft sends
`AUTO_RECONNECT`. It retains its listener; FreeRDP performs a fresh TLS connection
and supplies the saved reconnect cookie. Certificate pinning and the configured
server endpoint check remain enabled. No route, physical display, or system
network setting changes are involved.

`results.json` records the observed upstream TRX outcome separately from harness
acceptance. `evidence/` contains exact TRX, client, driver, and bridge records;
`environment.json` identifies the client and source revisions. No private keys or
executables are included.

| Case | Outcome and integration decision |
| --- | --- |
| `BVT_AutoReconnect_PositiveTest` | Passed standard and bridge: one launch, one deliberate interruption, two connections; bridge reports connected → reconnecting → connected. Add with `auto_reconnect: true`. |
| `S3_AutoReconnection_NegativeTest_AutoReconnectionFailed` | Passed standard and bridge with the added observation assertion: one launch, one interruption, two connections, and no peer disconnect during the 10 seconds following Auto-Reconnect Status. Add with `auto_reconnect: true` and `session-reconnect-observation.patch`; initial upstream-only results are superseded. |
| `S6_AutoDetection_NetworkCharacteristicsSync_After_AutoReconnect` | Both modes reconnect, but fail the protocol assertion: the client replies `RDP_RTT_RESPONSE` where Microsoft expects `RDP_NETCHAR_SYNC`. Keep out of the passing profile pending a protocol/implementation review; this is no longer an adapter-control blocker. |
| `BVT_HealthMonitoring_PositiveTest_SendHeartbeat` | Both modes establish the first connection and receive the heartbeat but time out waiting for a second connection. SDL does not register a `ServerHeartbeat` callback; the core parses the heartbeat without implementing missing-heartbeat reconnect policy. Requires client behavior investigation, not a larger control timeout. |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidEncryptFlag` | Initially passed both modes with default TLS options. This test does not require input injection. Its result must be repeated with the strict driver disconnection patch because the original helper can mistake an unrelated transport event for disconnection. |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidSignature` | TLS-inapplicable. Upstream returns nominal `Passed` without launching a client in both modes; the harness correctly rejects this no-op. It requires Standard RDP Security. |
| `S1_ConnectionTest_Disconnection_PositiveTest_ClientInitiated` | The upstream driver assumes a Shutdown Request PDU, whereas FreeRDP uses the permitted immediate MCS user-requested disconnect sequence. The narrowly scoped adapter correction passed both modes (one launch and one close action), with actual user-requested MCS PDU and peer-close verification. Add with `graceful_close: true`. |
| `BVT_ClientInputTest_FastPath`, `BVT_ClientInputTest_SlowPath` | Require actual keyboard/mouse input, synchronization, and suppress-output behavior. There is no SDL launcher input command; owned-process macOS event posting and Accessibility operations are not permitted in this execution context (`CGPreflightPostEventAccess` and `AXIsProcessTrusted` both false). Do not replace input with directly crafted RDP packets. |
| `S1_RDPEDISP_ResolutionChange_RestartGraphicsPipeline`, `S1_RDPEDISP_ResolutionChange_DeactivationReactivation` | Need a display-control adapter, controlled initial geometry, and the requested client display change. Current commands do not expose owned-window geometry control; native Accessibility control is unavailable. Keep pending a deterministic fixture. |
| `S3_RDPEDISP_MonitorAdditionRemoval_RestartGraphicsPipeline`, `S3_RDPEDISP_MonitorAdditionRemoval_DeactivationReactivation` | Require a controlled monitor inventory change, including correct monitor-count/layout PDUs. No isolated topology fixture is available to this adapter. Existing host display settings were not changed. |

## Graceful-close compatibility correction

[MS-RDPBCGR §1.3.1.4.1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/27915739-8f77-487e-9927-55008af7fd68)
permits immediate MCS Disconnect Provider Ultimatum with reason `user requested`
and transport close; Shutdown Request is an alternative sequence. FreeRDP's
`libfreerdp/core/freerdp.c` implements the immediate sequence when the client
cancels a connection. The pinned Microsoft adapter unconditionally waits for a
Shutdown Request and therefore cannot recognize that legal behavior.

`patches/session-graceful-disconnect.patch` adds that legal alternative to the
Microsoft adapter. It requires an actual MCS disconnect PDU with reason 3, followed
by the peer transport-close event. A process exit, unrelated packet, decoder
exception, or server-initiated close cannot satisfy the new branch. The original
Shutdown Request handling remains available.

The per-case `graceful_close` adapter action sends authenticated bridge `cancel`
or SIGTERM to the owned standard client PID. SDL registers a SIGTERM handler that
aborts the session and posts its quit event. This action never escalates to a
force-kill or waits for process exit before the driver can inspect the wire.
Qualification passed with .NET SDK 8.0.425: standard 5.22 seconds, bridge 5.13 seconds. Two deliberately abrupt-exit control probes both failed (18.23/18.38 seconds), demonstrating that transport closure without the required MCS PDU is rejected. `probe_transport_only_exit.py` reproduces this expected-failure control and is not used by the passing suite. Normal final cleanup remains separate. The runner also requires exactly one
actual graceful-close action, so prelaunch cleanup is not counted as coverage.

## Reconnect failure-status observation

The pinned upstream `S3_AutoReconnection_NegativeTest_AutoReconnectionFailed`
method described a continued-connection check in step 6 but ended immediately
after sending Auto-Reconnect Status. The initial passing result therefore proved
only that the server delivered that status after a successful reconnect.

`patches/session-reconnect-observation.patch` completes the described observation:
after status delivery it asserts that `WaitForDisconnection(waitTime)` is false.
The configured interval is 10 seconds. It depends on
`patches/driver-disconnect-verification.patch`, so ordinary incoming data is
buffered while waiting and exceptions cannot masquerade as a disconnect result.
Both standard (19.49 seconds) and bridge (20.52 seconds) passed the strengthened
case with one client launch, one induced interruption, and two connections.

A negative oracle control deliberately closed its own bridge client two seconds
after the second connected event. It failed specifically at the new assertion,
confirming that the test detects disconnection during the observation interval.
`probe_reconnect_early_disconnect.py` reproduces that expected-failure control;
normal suite runs never use it. This bounded check does not claim eventual
successful login or recovery after the server failure status.

## Local verification

`python3 -B test_session_controls.py` exercises the real loopback relay, reconnect
forwarding, bounded shutdown, rejection of invalid/no-connection/repeated
interruption requests, actual command routing, and graceful-close action bounds.
Initial run: eight checks passed. The initial protocol qualification consisted of
12 executions: four reconnect passes, two provisional encryption-flag passes,
four real protocol failures, and two nominal upstream passes rejected because no
client launched. Later combined-driver qualification is authoritative for profile
membership.

Reproduce selected investigations with `python3 investigations/2026-10-15-extended-batches/session/probe.py OUTPUT CASE...`.
The script defaults to both modes; `PROBE_MODE`, `PROBE_CLIENT`, `PROBE_SUITE`, and
`PROBE_BIN` select isolated inputs. It never modifies the shared prepared suite.

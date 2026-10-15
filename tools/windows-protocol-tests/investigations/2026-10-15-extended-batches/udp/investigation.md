# Modern UDP extended-suite batch

Date: 2026-10-15. Microsoft source: `29ddb4238a5443b7499e82934c90cc898861c5e7`. Final qualification uses .NET SDK 8.0.425 / runtime 8.0.31 on macOS arm64. The FreeRDP production engine is unchanged.

All three modern RDPEUDP2 cases pass with the three saved Microsoft driver patches, in both standard and bridge modes. Each execution produced its exact-name Passed TRX, launched one client, exited successfully, and had no adapter errors. Use the default case options; all three belong in extended, not quick. [Structured results and hashes](qualification.json) preserve the six execution records.

| Test | Observed coverage |
|---|---|
| `S3_DataTransfer_V2_ClientReceiveData` | Negotiates v3, establishes TLS/multitransport, sends DATA, receives its acknowledgement. |
| `S3_DataTransfer_V2_DataPacketWithAck` | Sends DATA with a mandatory real piggyback ACK and the test's explicit AckOfAcks payload, then receives an acknowledgement. The fixture repeats an ACK for observed handshake DATA if no fresh ACK is pending. |
| `S3_DataTransfer_V2_AcknowledgeLostPacket` | Sends first/third packets, checks the ACKVEC identifies the missing middle sequence, sends the middle packet, then checks the recovery ACK covers it. |

## Driver repairs

[RDPEUDP2 driver patch](../../../patches/udp-rdpeudp2-driver.patch) retains the original receipt, missing-sequence and connection assertions. It makes these corrections:

1. The first two tests log an outgoing ACK only when it exists. Previously logging dereferenced a nullable field before the receipt assertion could run. The original SDK already defines that outgoing ACK as optional. All received-packet null assertions now execute before dereferencing those packets, so failures produce assertion diagnostics.
2. Creating DATA considers only already received packets for an optional piggyback ACK. Previously each outgoing packet waited 20 seconds for unrelated incoming DATA. This changes no timeout for an expected protocol response.
3. ACKVEC decoding starts the vector at byte 3 when the optional timestamp/gap fields are absent, and byte 7 when present. The unchanged loss probe reproduced `ArgumentOutOfRangeException` at the hard-coded byte-7 offset. The repaired decoder then observes the client's correct missing sequence. This follows [MS-RDPEUDP2 2.2.1.2.6](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/43183820-771d-4a00-89d6-58a3ecc80a78).
4. Expected ACK matching recognizes cumulative acknowledgements using the protocol's bounded 16-bit sequence window, including wraparound. After receipt of sequences 6 and 8 followed by missing 7, the client returns ACK(8). That acknowledges 7 too: [MS-RDPEUDP2 3.1.5.6](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/32be8113-cdb4-4843-809c-3fa3aa886971) defines the acknowledgement as confirming earlier sequences. Previously the SDK checked only the explicit sequence/timestamp entries and incorrectly rejected this valid recovery ACK. Stale ACKs and the ambiguous half-window boundary remain rejected.

[UDP listener shutdown patch](../../../patches/udp-listener-shutdown.patch) wakes the driver's own listener with an empty local datagram after its exit flag is set, before closing its socket and joining the receiver. The original path hung in cleanup even after the receive assertion succeeded. Diagnostic instrumentation isolated the hang to `RdpeudpServer.Stop`; a native sample showed repeated socket shutdown/close work. The listener wakeup fixes the observed macOS shutdown hang. The empty datagram is a local teardown operation after test assertions, not test traffic sent to the FreeRDP client.

[Mandatory piggyback fixture patch](../../../patches/udp-piggyback-ack-fixture.patch), applied after the RDPEUDP2 driver patch, preserves the intended DATA-with-ACK scenario. The SDK retains an ACK constructed for observed client DATA. When the handshake has already drained all pending acknowledgements, this specific test repeats that genuine ACK with an updated time gap. It asserts an ACK exists and sets the wire ACK flag before sending DATA+ACK+AOA. It never invents a client sequence. The final logs show peer sequences 5 (standard) and 4 (bridge). No other test or automatic transport path requests repeated ACKs.

An exploratory TLS-inner-stream disposal change did not fix teardown and was removed. Diagnostic logging changes were also removed. The final three patches modify four Microsoft files only and apply cleanly to the pinned revision.

## Validation and evidence

The final supported-SDK run passed all six protocol executions and all 15 standalone driver regression checks. The standalone checks cover timestamp-present/absent ACKVEC, exact/cumulative/stale/wrapped/ambiguous ACKs, non-ACK rejection, immediate DATA without pending ACK, pending-ACK consumption, observed-only repeated ACKs, and listener shutdown. They are executable with:

```sh
python3 regressions.py /absolute/path/to/prepared-suite/Bin
```

The helper builds in a temporary directory, pins stable .NET 8, and imposes a deadline on the shutdown check. [Regression output](evidence/regressions.log) and complete [standard](evidence/standard/) / [bridge](evidence/bridge/) protocol evidence are retained without private keys or suite binaries. [Baseline loss evidence](evidence/baseline-loss/) records the decoder failure; [intermediate teardown evidence](evidence/intermediate-teardown/) records the successful receive assertion followed by the blocked stop.

Early exploratory and first qualification builds accidentally selected installed SDK 11 RC while targeting/running net8. Those results are not the final qualification record. The saved patches were subsequently rebuilt and all six protocol executions repeated under SDK 8.0.425. A first supported-SDK run was then superseded by another full six-execution run after strengthening the mandatory piggyback-ACK fixture. Only that final supported-SDK run populates `qualification.json`.

For isolated reproduction, `probe.py` accepts a new output directory and optional test names, uses the existing harness, and defaults to the normal FreeRDP test binary. `PROBE_BIN` overlays separately prepared Microsoft assemblies, `PROBE_MODE=bridge` selects bridge mode, and `PROBE_CLIENT` selects another client binary. No modifications to the original prepared suite are required.

## Other multitransport candidates

| Case | Disposition |
|---|---|
| `S1_Connection_SoftSync_Reliable` | Keep excluded from this v3-only profile. Calls the same automatic UDP handshake as the original legacy initialization probe. |
| `S2_AutoDetect_RTTMeasure_Reliable` | Keep excluded from this v3-only profile for the same legacy handshake prerequisite. |
| `S2_AutoDetect_ReliableBandwidthMeasure` | Keep excluded from this v3-only profile for the same legacy handshake prerequisite. |
| `S3_Tunneling_StaticVirtualChannel_PositiveTest` | Defer. Requires RDPEFS over a dynamic `rdpdr` channel. The method advertises UDP preference but does not explicitly establish or assert a UDP tunnel, so a nominal pass would not prove UDP channel traffic. Requires a separate fixture/coverage review. |

The first three share [RdpemtTestSuite.EstablishUDPConnection](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/TestSuite/RDPEMT/RdpemtTestSuite.cs#L203), which calls `RdpeudpServer.Accept`; its automatic handshake selects the legacy version. The fourth calls [EstablishTunnelingStaticVCTrafficConnection](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/TestSuite/RDPEMT/RdpemtTestSuite.cs#L327). These were source-audited, not newly executed. No legacy case was silently converted into a modern-version scenario. The broader legacy-security and Windows-only-driver exclusions remain as documented in the earlier investigation.

# Extended protocol batches — 2026-10-15

Extended now contains **115 cases**; quick remains **6**. We added **28** cases
and removed **5** earlier false positives. Every enabled case passed in
standard and bridge modes. No production FreeRDP source was changed.

| Batch | Added | Coverage / limits |
| --- | ---: | --- |
| Graphics | 16 | H.264 support + 12 AVC420/AVC444/AVC444v2 streams, compression, two segmentation cases; protocol/frame ACKs, not pixel accuracy |
| Static drive channel | 5 | Four positive RDPEFS exchanges and invalid MCS-length rejection; no file I/O or explicit compression/chunk-boundary assertions |
| Modern UDP | 3 | Receive ACK, required observed-data piggyback ACK, loss ACKVEC and recovery |
| Session lifecycle | 3 | Positive reconnect, failure status with ten-second observation, graceful client close with wire evidence |
| Slow-path malformed traffic | 1 | Invalid encryption flag with actual disconnect verification |

The work used isolated source/builds for investigations, then the normal runner
with combined driver builds. [Qualification](../../validation-batches.json)
retains all 246 executions, including failed old cases
and superseded observations. The 230 final selected case/mode results all pass.
The first two batches use five patches; the final batch repeats all modern UDP
and negative reconnect cases with the final two added fixture/assertion patches.
This is a batch qualification, not one full run of the final seven-patch build.

Thirty-five Python harness checks and 22 checks against the actual final C#
driver passed. The disconnect regression detects the original SDK defect;
abrupt-exit and post-status early-disconnect controls fail as expected. See
[final regression output](final-regressions/).

## Corrected false positives

Microsoft's adapter treated any `InvalidOperationException` from disconnect
waiting as success. The SDK generated that exception for ordinary received
traffic. The repair buffers ordinary packets and requires the matching actual
peer-close event; unrelated decoder failures no longer count as success.
The old qualification record is historical and does not establish these cases:

| Removed from extended | Reason |
| --- | --- |
| `S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidMCSLength` | Required peer-disconnect event not observed with the corrected verifier. |
| `S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidPDULength` | Required peer-disconnect event not observed with the corrected verifier. |
| `S1_ConnectionTest_Disconnection_NegativeTest_MCSDisconnectProviderUltimatum_InvalidTPKTLength` | Required peer-disconnect event not observed with the corrected verifier. |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidMCSLength` | Required peer-disconnect event not observed with the corrected verifier. |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidTPKTLength` | Required peer-disconnect event not observed with the corrected verifier. |

## Candidates left out

| Area | Result / reason |
| --- | --- |
| GFX capability, compression and segmentation negatives | All 14 failed to pass both modes with the original required disconnect behavior. Both capability negatives also fail in both modes with strict verification. No assertion was relaxed to accept channel-only rejection. |
| AVC444 separated luma/chroma | Frame 2 was not acknowledged in either mode; exact cause remains unconfirmed. |
| Drive invalid TPKT length | Failed in both modes with strict verification; original apparent success was a false positive. |
| Heartbeat | Connected session did not auto-reconnect; SDL lacks the heartbeat timeout callback. |
| Network characteristics after reconnect | Reconnect succeeds, but RTT_RESPONSE arrives instead of required NETCHAR_SYNC. |
| Input | Two actual input BVTs require native input injection; Accessibility/event-posting permission is unavailable. The encryption/signature negatives do not require input. |
| Display | Needs an isolated resolution/orientation/monitor-topology fixture and appropriate SUT controls. Physical displays were not changed. |
| NLA/CredSSP | Needs a separate Windows Microsoft driver; keep it outside this macOS/TLS job. |
| Legacy security and UDP | Standard RDP Security fields are absent from TLS; legacy v1/v2 UDP is incompatible with this fork's v3-only transport. These exclusions are configuration-specific. |

The [interactive catalog](../../test-catalog.html) covers all 432 source methods
and records a reason for every unimplemented case. Detailed batch investigations:
[drive](drive/README.md), [graphics](graphics/README.md),
[UDP](udp/investigation.md), [session](session/investigation.md).

The ordered [driver repairs](../../patches/README.md) are hash-pinned in
`suite.json` and applied only to a temporary Microsoft source export. The normal
runner rejects stale prepared builds. Source revision, SDK, client hash, patch
hashes and per-case options are retained in the qualification record. Old raw
investigations remain visible and are explicitly superseded where necessary.

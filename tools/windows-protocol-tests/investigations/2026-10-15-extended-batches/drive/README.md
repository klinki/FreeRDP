# Static drive-channel batch qualification

Five Microsoft static-channel cases passed in standard and bridge modes on 2026-10-15 after driver repairs and strict disconnect verification. The two previously blocked BVT cases now complete the original RDPEFS traffic sequence. One further drive negative and both GFX capability negatives failed their original connection-drop expectation and must not be added as passing tests. The legacy signature case remains inapplicable to the TLS profile.

[qualification.json](qualification.json) lists the five extended candidates, required `redirect_drive: true` options, and all 16 strict executions: 10 Passed and six Failed. Every pass used the existing runner's exact-name single-result TRX check, successful driver exit, one client launch, no control-adapter errors, and bridge hello verification where applicable. Production FreeRDP behavior and the client binary were unchanged.

## Microsoft driver repairs

Both patches apply together cleanly to pinned revision `29ddb4238a5443b7499e82934c90cc898861c5e7`.

[Drive prerequisites](../../../patches/drive-static-channel-prerequisites.patch):

1. Locate the channel-name NUL with the literal character overload instead of culture-sensitive string search, preserving the actual `rdpdr` name on macOS/.NET.
2. Serialize and decode GENERAL_CAPS_SET V1 without the V2-only SpecialTypeDeviceCap field: 40 total bytes for V1, 44 for V2.
3. Advertise RDPDR_USER_LOGGEDON_PDU in the server general capability, matching the user-logon packet the driver already sends when the client supports it.

[Strict disconnect verification](../../../patches/driver-disconnect-verification.patch) fixes a separate false-pass defect. The SDK's ExpectDisconnect consumed a single transport event, throwing InvalidOperationException for ordinary ReceivedPacket traffic. The adapter treated every InvalidOperationException, including decoder failures, as a successful disconnect. The repaired SDK buffers ordinary packets and keeps waiting within the original deadline; only a matching Disconnected event succeeds. The adapter no longer swallows other exceptions. The TLS receive loop already converts EOF and IO errors, including connection resets, into Disconnected events.

No test method, negative packet, or expected disconnect assertion was weakened. This stricter verifier exposes cases whose earlier Passed results did not establish a disconnect. The [earlier drive investigation](../../2026-10-15-drive-udp/investigation.md) documents failures before the prerequisite repairs.

## Strict results

| Case | Standard | Bridge | Decision |
| --- | --- | --- | --- |
| BVT_StaticVirtualChannel_PositiveTest_CompressionNotSupported | Passed | Passed | Extended |
| BVT_StaticVirtualChannel_PositiveTest_VCChunkSizeNotPresent | Passed | Passed | Extended |
| S7_StaticVirtualChannel_PositiveTest_CompressionSupported | Passed | Passed | Extended |
| S7_StaticVirtualChannel_PositiveTest_VCChunkSizePresent | Passed | Passed | Extended |
| S7_StaticVirtualChannel_NegativeTest_InvalidMCSLength | Passed | Passed | Extended |
| S7_StaticVirtualChannel_NegativeTest_InvalidTPKTLength | Failed | Failed | Keep excluded; no expected disconnect |
| RDPEGFX_CapabilityExchange_Negative_IncorrectCapsDataLength | Failed | Failed | Keep excluded; no expected disconnect |
| RDPEGFX_CapabilityExchange_Negative_IncorrectVersion | Failed | Failed | Keep excluded; no expected disconnect |

The GFX probes used the previously qualified graphics configuration and capability mask `0x200`. The strict failures time out on the original connection-drop assertion. Client logs show cleanup initiating the eventual termination. In contrast, InvalidMCSLength logs a channel/header-length error followed by a real disconnect, and passes the strict verifier in both modes. This is an observed mismatch with the selected test expectation; it is not a claim that every non-Windows client must disconnect for these malformed inputs.

The [initial qualification](qualification-initial.json), before the disconnect repair, reported all six drive cases Passed. That record is explicitly superseded: its InvalidTPKTLength results are invalid evidence of a disconnect. Original and strict raw logs are retained so the change in outcome is reviewable.

## Coverage limits

The positive cases complete RDPEFS announce, client name, capability and device-list traffic using an empty redirected scratch folder. They do not perform file read/write operations. The upstream positive methods also do not explicitly assert compression flags or chunk-size boundaries and ignore the RDPEFS decoder's boolean results. These are protocol-exchange smoke tests, not exhaustive compression, fragmentation or filesystem conformance coverage. Original assertions were preserved.

`S7_StaticVirtualChannel_NegativeTest_InvalidSignature` requires Standard RDP Security signature fields. With TLS, its upstream guard returns before launching a client; that result is not integrated or counted as a pass.

## Reproduction

Apply both patches to an isolated copy of the pinned Microsoft source. Publish `TestSuites/RDP/Client/src/TestSuite/RDP_ClientTestSuite.csproj` with .NET 8 and `-p:TestSuiteRoot=<isolated-source>`. [probe.py](probe.py) accepts `PROBE_BIN=<published-binaries>` and a new output directory. It overlays those binaries in a private suite copy and runs all eight cases in both modes with the existing runner checks. Three cases are intentionally expected to fail during this investigation; the helper does not change their assertions.

The qualification used `/private/tmp/wpts-drive-batch-source` and `/private/tmp/wpts-drive-batch-output/strict-bin`; these temporary directories are not runtime dependencies. [Raw evidence](evidence/) and [hashes](evidence-sha256.json) omit generated certificates, private keys, suite copies and binaries.

## Driver regression checks

The [focused C# regression](regression/README.md) executes the actual Microsoft SDK assembly, transport event queue and TLS receive-loop conversion. All seven checks pass with the repair; the original assembly fails both ordinary-traffic checks. This also verifies that actual matching disconnects, EOF and connection resets remain accepted while unrelated decoder exceptions and other-session disconnects are rejected.

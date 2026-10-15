# Static drive-channel and reliable-UDP investigation

> Historical investigation at the revision below. The [subsequent batches](../2026-10-15-extended-batches/README.md) repair the driver and qualify selected drive/UDP cases; the original failures remain preserved here.

Date: 2026-10-15. Microsoft source: `29ddb4238a5443b7499e82934c90cc898861c5e7`. FreeRDP branch: `codex/windows-protocol-smoke`, revision `50193de1cf31bdd1f3b2db1ba6dad6adb85a2e9c`. Environment: macOS 27.0.1 arm64, .NET SDK 8.0.425. Binary and source hashes are in [hashes.json](hashes.json).

No tests were added to quick or extended. The default Microsoft source, prepared suite and production FreeRDP code were unchanged. The patches below are isolated investigation artifacts, not applied by `prepare.py`. The investigation ran 12 targeted executions; none passed end to end. Intermediate fixes eliminated specific driver faults but do not establish successful drive testing.

## Disposition

| Microsoft case | Status | Finding and next action |
|---|---|---|
| `BVT_StaticVirtualChannel_PositiveTest_CompressionNotSupported` | Not implemented; driver blockers | Valid RDPDR channel exists. Microsoft channel-name lookup and V1 capability serialization are defective. Isolated repairs advance the test to a logon capability mismatch and final device-list timeout. Repair that driver sequence and rerun both modes before enabling. |
| `BVT_StaticVirtualChannel_PositiveTest_VCChunkSizeNotPresent` | Not implemented; driver blockers | Same failures and progression as the preceding case. |
| `S1_Connection_Initialization_InitialUDPConnection` | Inapplicable to this fork's current UDP transport | Test negotiates legacy RDPUDP v1; this fork requires version 3/RDPEUDP2. A positive v1 test cannot pass without adding legacy client transport support. |
| `S1_Connection_Initialization_InitialReliableConnection` | Inapplicable to this fork's current UDP transport | The SDK's automatic handshake negotiates v1; the client explicitly declines it. Use actual RDPEUDP2 cases for this implementation. |
| `S3_DataTransfer_V2_ClientReceiveData` (supplementary probe) | Not implemented; incomplete test-driver execution | RDPEUDP2, TLS and multitransport tunnel establish successfully. Test then enters cleanup before data acknowledgement verification, and driver never emits a TRX result. Nullable ACK handling and cleanup need investigation. |

“Inapplicable” is scoped to the current macOS TLS/v3-only profile and this client build. It does not mean the Microsoft tests are invalid or that every RDP client must reject v1/v2.

## Drive finding 1: channel-name lookup

The two original failures were reproduced with the unchanged prepared suite, `SupportRDPEFS=true`, and the harness's isolated empty redirected folder. The client loads the drive service; the Microsoft diagnostic adapter confirms the negotiated channel name bytes `72-64-70-64-72-00-00-00` (`rdpdr\0\0\0`) and channel ID `1004`.

In `RdpbcgrAdapter.Server_MCS_Connect_Response`, `name.IndexOf("\0")` uses culture-sensitive string search. In this macOS/.NET environment that search yields zero; its `len <= 0` fallback selects seven characters and stores `RDPDR\0\0`. Later `GetStaticVirtualChannelId("RDPDR")` returns zero because the dictionary key contains the extra NUL characters. The protocol channel was present throughout.

[Diagnostic evidence](evidence/drive-diagnostic/standard-1/ptf-debug.log) records both channel bytes and dictionary keys. [The one-line patch](rdpdr-channel-name.patch) switches to the character overload, which performs a literal character search. Applying that patch advances both tests beyond the lookup in both standard and bridge modes; the next failure is recorded below.

[Microsoft adapter source](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/Adapter/RDPBCGR/RdpbcgrAdapter.cs#L671)

## Drive finding 2: malformed V1 general capability

After the lookup repair, both cases fail in both modes with the client diagnostic `CAP_GENERAL_TYPE::CapabilityLength expected 32, got 36`. The subsequent Microsoft TLS write exception is a consequence of the client closing the invalid channel/session; it is not evidence of a TLS-negotiation problem.

The RDPEFS adapter constructs `GENERAL_CAPS_SET(CAPABILITY_VERSION.V1)`. Its SDK constructor always sets total length to 44, and its encoder always writes `SpecialTypeDeviceCap`. That field belongs only to V2: V1 requires 40 total bytes, including the eight-byte header. FreeRDP's length diagnostic reports the remaining payload, 32 versus 36. This agrees with Microsoft's [General Capability Set specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpefs/06c7cb30-303d-4fa2-b396-806df8ac1501).

[The SDK patch](rdpefs-v1-capability.patch) selects the length and optional field according to the version, on both encoding and decoding. It preserves the V1 scenario and all original test assertions. With both patches, all four executions advance through the capability exchange to the final device-list wait. This is validation of the two prerequisite repairs, not an end-to-end pass.

Evidence: [standard case 1 client log](evidence/drive-patched-standard/standard-1/client-1.log), [standard summary](evidence/drive-patched-standard-summary.json), [bridge summary](evidence/drive-patched-bridge-summary.json).

[Microsoft capability factory](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/Adapter/RDPEFS/RdpefsAdapter.cs#L269) · [SDK serializer](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPEFS/Types/RdpefsTypes.cs#L1094)

## Drive remaining blocker: user-logon negotiation

After both isolated repairs, both cases still fail in standard and bridge modes while waiting for the final device-list PDU. The client logs `Checking ExtendedPDU::RDPDR_USER_LOGGEDON_PDU, client supported, server not found`.

The driver's server capability advertises only device removal and display-name flags, omitting `RDPDR_USER_LOGGEDON_PDU`. Later it sends `DR_CORE_USER_LOGGEDON` when the client advertises that flag, without checking its own server capability. FreeRDP's channel-state guard requires support on both sides and ignores this inconsistent packet. This identifies the remaining negotiation mismatch; no production FreeRDP change or further driver repair was attempted. First make the driver's advertised flags and transmitted logon sequence agree, then repeat the original assertions in both modes. Additional behavior beyond that point remains unverified.

Evidence: [client log](evidence/drive-two-fixes-standard/standard-1/client-1.log), [driver sequence](evidence/drive-two-fixes-standard/standard-1/ptf-debug.log), [standard summary](evidence/drive-two-fixes-standard-summary.json), [bridge summary](evidence/drive-two-fixes-bridge-summary.json).

All seven `S7_StaticVirtualChannelTest.cs` methods share the channel-name lookup. The four positive cases share the general-capability traffic generator. The invalid-TPKT/MCS-length cases send only an announce packet, so do not necessarily encounter the general-capability bug. Only the two named BVT cases were executed here. `S7_StaticVirtualChannel_NegativeTest_InvalidSignature` is additionally inapplicable to TLS: it returns before launching the client unless RDP Standard Security is selected. Other S7 cases must not be called passing based on this investigation.

## Reliable UDP: version mismatch, not an unexplained slow test

The retained original client logs for both failed probes explicitly say `server negotiated RDPUDP v1, need v3 (RDPEUDP2)`, followed by an async-establishment failure and decline. The original RDPUDP initialization test calls `EstablishUDPConnection` with no version override. Its server SYN/ACK therefore does not select version 3. The reliable RDPEMT case calls `RdpeudpServer.Accept`, whose automatic `SendSynAndAckPacket()` also supplies no version. This results in the legacy v1 handshake.

`libfreerdp/core/rdpeudp.c` advertises version 3 and explicitly returns failure if the selected version is not `RDPUDP_PROTOCOL_VERSION_3`. The corresponding 45-second driver deadline is downstream of this known incompatibility, not evidence of missing UDP connectivity or a generally broken FreeRDP transport. Repeating the same legacy probes with a longer deadline would not change that mismatch.

Evidence: [original UDP client log](evidence/original-legacy-udp/06-standard/client-1.log), [original reliable-tunnel client log](evidence/original-legacy-udp/07-standard/client-1.log).

[Microsoft manual handshake](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/TestSuite/RDPEUDP/RdpeudpTestSuite.cs#L228) · [automatic handshake](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPEUDP/Server/RdpeudpServerSocket.cs#L46)

A version-3 modification would be a different scenario, not a passing execution of these original legacy tests. Keep those cases excluded from this profile; validate the Microsoft `RDPEUDP2` cases separately.

## Supplementary RDPEUDP2 probe

One unchanged `S3_DataTransfer_V2_ClientReceiveData` execution used the default profile. The client logged successful RDPEUDP2 connection, TLS over UDP, tunnel creation and async `S_OK`. Microsoft logged successful tunnel creation, then `The DataSeqNum of next valid RDPEUDP2 packet is 6`, followed by cleanup. It never completed the test or produced TRX before the 80-second outer deadline. Therefore the record proves a working v3 tunnel, not a passed data-transfer test.

The next source statement dereferences `nextUdpPacket.ACK.Value` unconditionally. `Rdpeudp2ProtocolHandler.CreateDataPacket` legitimately omits that optional ACK when none is available. This is a source-supported explanation to investigate, but the failed run produced no exception stack or TRX, so the exact exception and cleanup hang are not confirmed. No ACK assertion was removed or weakened, and no other RDPEUDP2 case was run.

Evidence: [client log](evidence/baseline-live/standard-3/client-1.log), [Microsoft log](evidence/baseline-live/standard-3/ptf-debug.log), [summary](evidence/baseline-live-summary.json).

[Microsoft v3 test](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/TestSuite/RDPEUDP/S3_DataTransfer_V2.cs#L46) · [optional ACK construction](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPEUDP2/Rdpeudp2ProtocolHandler.cs#L486)

## Reproduction and artifact limits

[probe.py](probe.py) imports the unchanged local runner, copies its prepared suite into a new output directory, creates a temporary certificate and runs named cases with existing assertions. The only non-default client setting for drive cases is the existing redirected scratch-folder option; diagnostic log filters are enabled. `PROBE_MODE=bridge` selects bridge mode; `PROBE_BIN` overlays a separately built diagnostic/patched Microsoft assembly directory onto that per-run suite copy. Both patches pass `git apply --check` against the pinned clean Microsoft checkout.

The saved helper resolves the runner relative to its repository location and
defaults to `build/protocol-tests/client/SDL/SDL3/sdl-freerdp`; set `PROBE_CLIENT`
to use another executable. It must live under this investigation directory.

The isolated build command was:

```sh
dotnet publish /private/tmp/wpts-investigation-source/TestSuites/RDP/Client/src/TestSuite/RDP_ClientTestSuite.csproj \
  -c Release -o /private/tmp/wpts-investigation-output/patched2-bin \
  -p:TestSuiteRoot=/private/tmp/wpts-investigation-source \
  --no-restore --disable-build-servers -m:1
```

Example baseline reproduction (output path must not exist):

```sh
python3 probe.py /private/tmp/drive-investigation \
  BVT_StaticVirtualChannel_PositiveTest_CompressionNotSupported \
  BVT_StaticVirtualChannel_PositiveTest_VCChunkSizeNotPresent
```

[Machine-readable results](investigation.json) retain all targeted outcomes. Evidence copies omit generated private keys and full suite binaries. The local full runs remain under `/private/tmp/wpts-investigation-output`; these temporary paths are diagnostic working files, not stable repository dependencies. No upstream issue/PR was sent and no changes were committed by the investigation agent.

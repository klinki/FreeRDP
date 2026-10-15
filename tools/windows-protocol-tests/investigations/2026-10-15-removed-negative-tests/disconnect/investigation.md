# Invalid TPKT length in MCS disconnect: Microsoft fixture underflow

**Classification: confirmed Microsoft test-generator defect; this failure does not demonstrate a FreeRDP bug.** The documented negative scenario passes in standard and bridge modes after a minimal generator repair, with the original disconnect assertion and native client unchanged. This investigation does not re-enable the case or alter the 115-test extended profile.

Case: `S1_ConnectionTest_Disconnection_NegativeTest_MCSDisconnectProviderUltimatum_InvalidTPKTLength`.

## What the test sends

The test says that the TPKT length will be **less than** the actual packet length. In fact, it sends this nine-byte plaintext buffer through the established TLS stream:

```text
03 00 ff ff 02 f0 80 21 80
      ^^^^^ declared TPKT length = 65535; actual bytes written = 9
```

The defect is deterministic:

1. Microsoft's factory initializes `tpktHeader.length` to zero. Zero is a sentinel requesting automatic length calculation during serialization.
2. The negative adapter performs `ultimatunPdu.tpktHeader.length--` on that unsigned 16-bit field, underflowing to `0xffff`.
3. `RdpbcgrUtility.ToBytes` repairs the length only when both serialized length bytes are zero. It preserves `ff ff`.
4. `RdpbcgrServerTransportStack.SendPacket` writes the resulting bytes unchanged to the TLS stream.

The diagnostic logs capture the bytes immediately before `SslStream.Write` and the successful return from that write. They are an instrumented write observation, not a separate packet capture. Both modes observe identical bytes. The paired delayed-tail runs below establish that the client receives and reassembles incomplete frames.

The original test calls `ServerDeactivateAll`, then `Server_MCSDisconnectProviderUltimatum` directly, then waits for the client to close. That direct helper **does not close the server transport**. The separate `ServerInitiatedDisconnect` helper does close it, but this test does not call that helper. The open stream and missing 65,526 bytes therefore persist throughout the configured ten-second assertion window. Test cleanup closes the stream afterward.

Pinned Microsoft sources:

- [Test definition, steps, and unchanged assertion](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/TestSuite/RDPBCGR/S1_Connection_DisconnectionTest.cs#L20)
- [Factory initializes length zero](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPBCGR/Server/RdpbcgrServer.cs#L2034)
- [Adapter decrements the zero field](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/Adapter/RDPBCGR/RdpbcgrAdapter.cs#L1069)
- [Serializer repairs only zero](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPBCGR/Utility.cs#L947), [PDU serialization](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPBCGR/Types.cs#L8642), [TLS write](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPBCGR/Server/RdpbcgrServerTransportStack.cs#L668)
- [Different helper that does close the server transport](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/Adapter/RDPBCGR/RdpbcgrAdapter_Sequences.cs#L220)

## Why waiting is expected

TPKT permits total lengths from 7 through 65,535 bytes. TCP supplies a byte stream; the receiver uses the TPKT length to delimit packets. TLS also permits application messages to span records, so a complete TLS record containing nine bytes does not establish a complete TPKT. See [RFC 1006 §6](https://www.rfc-editor.org/rfc/rfc1006.html#section-6) and [RFC 5246 §6.2.1](https://www.rfc-editor.org/rfc/rfc5246.html#section-6.2.1).

Microsoft's [MS-RDPBCGR §3.1.5.1.2](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/f10e972d-7b77-4fa5-999f-08406fe86284) requires examining embedded lengths and recommends dropping a discrepant packet. It also says the recipient expects the sender to close after a disconnect ultimatum. [§3.1.5.1.1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/3efcb3f7-8fa5-488b-8388-bf4d32004b8c) requires that sender-side close. These rules do not establish an application-frame completion boundary at a TLS record boundary or require a particular partial-frame timeout.

In this FreeRDP build, [transport.c](../../../../../libfreerdp/core/transport.c) lines 1018–1043 accept 65,535 as a valid framing length; lines 1167–1192 wait for the declared remaining bytes before calling the RDP parser. For the corrected eight-byte frame, [rdp.c](../../../../../libfreerdp/core/rdp.c) lines 560–618 dispatch the disconnect ultimatum to [mcs.c](../../../../../libfreerdp/core/mcs.c) line 1411, where the missing final reason byte is rejected. Both client logs show that exact rejection. This is the test's documented shorter-length scenario.

## Targeted results

Every row was run in **both standard and bridge modes**. The original test method and ten-second disconnect assertion were kept unchanged. The whole driver had a 70-second safety deadline; none reached it. All server endpoints used isolated loopback ports and temporary certificates.

| Variant | Declared length / delivered bytes | Both-mode observation | Meaning |
|---|---|---|---|
| Committed suite baseline | 65,535 / 9 | Strict assertion fails after its wait | Reproduces the removal; no client rejection before cleanup |
| Instrumented original | 65,535 / 9 | Same failure; exact bytes captured | Confirms the generator underflow survives the socket write |
| Documented short-length control | 8 / 9 | Strict assertion passes; MCS parser reports missing byte | Client rejects the intended malformed frame |
| Correct-length control | 9 / 9 | Normal remote logoff | Valid disconnect packet is processed |
| Incomplete small-frame control | 10 / 9, stream left open | Strict assertion times out | Waiting is independent of the large 65,535-byte value |
| Delayed completion control | 10 / 9, then one zero byte after 1 second | Normal remote logoff after final byte | Client advances after the TPKT frame is complete; diagnostic only |
| Original-size EOF control | 65,535 / 9, then server stream close after 1 second | Client logs network disconnect; driver throws disposal exception | EOF detection only; not client rejection or case qualification |
| Truncated valid-frame EOF control | 9 / 8, then server stream close after 1 second | Same EOF detection and driver exception | Incomplete-frame EOF detection only |
| Valid fragmentation control | 9 / 8, then final `80` byte after 1 second | Normal remote logoff after final byte | Client correctly accepts a packet split across writes |
| Minimal real generator repair | 8 / 9, original assertion | Strict assertion passes | Confirms a fixture repair is sufficient; no native change |

There were **20 case/mode executions**: two baseline, sixteen wire controls, and two using the actual generator repair. Raw TRX outcomes and their meanings are retained separately in [results.json](results.json). Successful correct-length, delayed-tail, and fragmentation controls do not count as malformed-packet coverage.

The EOF controls intentionally dispose the server's own `SslStream`; its receive thread consequently raises `ObjectDisposedException`, so their TRX results are failures. FreeRDP independently logs `Network disconnect!` approximately 0.13–0.22 seconds after the logged server close. The bridge's eventual `cancelled` terminal event is influenced by harness cleanup and is **not** proof of autonomous client termination. Neither EOF control qualifies the original case. No conclusion about indefinite waiting is drawn from a bounded ten-second observation.

## Repair and follow-up

[tpkt-short-length.patch](tpkt-short-length.patch) derives the actual serialized size before subtracting one and encodes that value in network byte order. It preserves the intended malformed packet and the strict peer-disconnect assertion. This patch was applied only in a temporary diagnostic build. Its resulting write is:

```text
03 00 00 08 02 f0 80 21 80
```

The actual patch passes the unchanged test in both modes. The recommended next step is to review and integrate this narrow Microsoft fixture correction, then qualify and restore the case to extended. There is no evidence from this case requiring a FreeRDP production change. The existing disconnect-verifier correction remains necessary; restoring its previous false-positive behavior would conceal this fixture defect again.

## Reproduction

Run from this directory with the committed prepared suite and an existing client build. Choose new temporary output paths for every run. The scripts never edit the original Microsoft checkout, production client, shared suite cache, or manifest.

```sh
python3 probe.py --variant baseline --scenarios original --output /private/tmp/disconnect-baseline-new
python3 prepare_diagnostic.py --source /private/tmp/wpts-source --output /private/tmp/disconnect-wire-build-new
python3 probe.py --variant wire-controls --bin /private/tmp/disconnect-wire-build-new/Bin --output /private/tmp/disconnect-wire-new
python3 prepare_diagnostic.py --source /private/tmp/wpts-source --output /private/tmp/disconnect-fixed-build-new --fix
python3 probe.py --variant generator-fixed --scenarios original --bin /private/tmp/disconnect-fixed-build-new/Bin --output /private/tmp/disconnect-fixed-new
```

The [wire diagnostic patch](diagnostic-wire.patch) logs only the final disconnect frame and varies its transport delivery according to `WPTS_DISCONNECT_SCENARIO`. `original` preserves the serialized bytes. `--fix` additionally applies the minimal adapter generator repair. The scripts require the same .NET 8, Python, OpenSSL, and client prerequisites as the main harness. Build outputs and test certificates stay in temporary directories.

Full provenance, native-client SHA-256, suite patch identities, and diagnostic assembly hashes are in [results.json](results.json). Evidence hashes are in [evidence-sha256.json](evidence-sha256.json). Only logs, TRX files, bridge events, and the narrowly scoped scripts/patches are preserved here; no private keys or compiled binaries are included.

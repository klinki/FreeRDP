# Slow-path length test investigation — 2026-10-15

The MCS test exposes a FreeRDP length-validation defect. The TPKT test has a Microsoft packet-generation defect and does not send the short-length packet described by its test plan. Neither result is caused by the launcher bridge.

| Exact Microsoft test | Stock client: standard / bridge | Control: standard / bridge | Classification |
| --- | --- | --- | --- |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidMCSLength` | Failed / Failed | Passed / Passed with private MCS consistency check | FreeRDP bug: an undersized MCS payload length is accepted on the global channel. |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidTPKTLength` | Failed / Failed | Passed / Passed after correcting only the emitted TPKT length | Microsoft fixture bug: zero length underflows to 65,535 instead of actual length minus one. Waiting for an incomplete stream record is not evidence of a client bug. |

These are server-to-client **pointer output** cases, despite their `SlowPathInputTest` class name. They establish a session, announce logon, and send one pointer-position update for coordinates `(50, 50)` on global channel 1003. No keyboard/mouse injection is involved.

## Serialized packet evidence

The diagnostic patch intercepts `RdpbcgrServerTransportStack.SendPacket` **after** `packet.ToBytes()` and **before** the bytes are written to the TLS stream. It logs the original and transmitted arrays. It does not serialize the packet twice or replace disconnect verification. The original seven prepared-suite patches remain applied.

All arrays contain 40 bytes; offsets below are zero-based. [wire.json](wire.json) preserves every captured packet with decoded lengths, and each run's TRX contains the same hex dump.

| Variant | TPKT bytes 2–3 | TPKT length | PER byte 13 | Declared MCS payload | Available payload |
| --- | --- | ---: | --- | ---: | ---: |
| Original MCS test | `00 28` | 40 | `19` | 25 | 26 |
| Original TPKT test | `ff ff` | 65,535 | `1a` | 26 | 26 bytes sent; the TPKT record remains incomplete |
| Corrected TPKT control | `00 27` | 39 | `1a` | 26 | Only 25 within the framed TPKT record |

Original MCS packet:

```text
03 00 00 28 02 f0 80 68 00 01 03 eb 70 19 1a 00
17 00 ef 03 ea 03 01 00 00 02 1a 00 1b 00 00 00
03 00 00 00 32 00 32 00
```

Original TPKT packet differs only at offsets 2–3 (`ff ff`) and 13 (`1a`). The corrected TPKT control changes offsets 2–3 to `00 27`, preserving every other byte.

## MCS: FreeRDP defect

Microsoft's `SendSlowPathOutputPdu` retains the first `TS_POINTER_PDU`. `SlowPathOutputPduEx.ToBytes` serializes it and decrements byte 13. For this small TLS packet the PER length has one byte, so that mutation correctly changes the user-data length from 26 to 25. The Share Control totalLength is still 26 (`1a 00` at offset 14).

FreeRDP `rdp_read_header` checks only that **at least** the declared MCS length is available. `rdp_recv_tpkt_pdu` subsequently parses global-channel Share Control PDUs from the entire remaining stream, so the 26-byte pointer payload is consumed despite declaring only 25. The final stream-consumed check sees no leftover data and cannot detect the earlier discrepancy.

Both stock-client runs reached the strict driver's ten-second disconnect assertion and failed. A private core library adding an exact MCS-payload consistency check made the unchanged test pass in both modes. Both native logs recorded:

```text
Diagnostic MCS payload mismatch: declared=25 actual=26
```

The private library also contains the sibling Demand Active diagnostic check, but this pointer-output packet is rejected by the MCS check before that state-specific code could matter. No shared client source or built library was replaced. This establishes a protocol-validation gap; it does not establish memory corruption or an exploit.

## TPKT: Microsoft fixture defect

`CreateSlowPathUpdataPdu` uses `FillCommonHeader` with default TPKT length zero. The adapter decrements that zero-valued `ushort`, producing `0xffff`. `RdpbcgrUtility.ToBytes` only fills the actual TPKT size when the field is zero, so the underflow survives serialization. The test plan says the length should be **less** than the transmitted data; the actual packet declares 65,535 bytes and sends only 40.

FreeRDP `parse_default_mode_pdu` accepts the legal TPKT length range 7–65,535. `transport_default_read_pdu` waits until the announced record is complete before calling the RDP parser. TLS records do not provide RDP message boundaries: a sender's single `Write` is not proof that its TPKT is complete. The driver leaves the stream open and waits for a disconnect; the client waits for the remaining 65,495 bytes. Cleanup eventually terminates the client. That timeout does not demonstrate acceptance of a malformed complete PDU.

The diagnostic control replaces the serialized TPKT size with `bytes.Length - 1` (39), which implements the test plan. The unchanged stock client then disconnects in both modes, and the strict test passes. Its log identifies the actual rejection:

```text
[rdp_read_header(libfreerdp/core/rdp.c:663)] invalid length, got 25, require at least 26
```

## Specification and sources

[MS-RDPBCGR 3.2.5.2](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/dac03011-f059-44d3-a30b-26f238ad21aa) requires examining embedded TPKT and MCS lengths for consistency; it recommends dropping a connection on discrepancies. Our configured strict driver asserts that recommended disconnect behavior. The MCS finding concerns the missing consistency check itself, not merely the choice to disconnect.

[RFC 1006 section 6](https://www.rfc-editor.org/info/rfc1006/) defines TPKT framing over a TCP byte stream and the packet length including its header. An incomplete declared record cannot be identified as complete merely from the size of one write or TLS record.

Pinned Microsoft source: `29ddb4238a5443b7499e82934c90cc898861c5e7`:

- [`S4_SlowPathInputTest.cs`](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/TestSuite/RDPBCGR/S4_SlowPathInputTest.cs): cases and test plans.
- [`RdpbcgrAdapter.cs`](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/Adapter/RDPBCGR/RdpbcgrAdapter.cs#L1421): pointer selection and TPKT mutation.
- [`RDPBCGRTypes.cs`](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/src/Adapter/RDPBCGR/RDPBCGRTypes.cs#L472): serialized MCS mutation.
- [`Utility.cs`](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/ProtoSDK/MS-RDPBCGR/Utility.cs#L947): zero-only TPKT size filling.
- FreeRDP [`rdp.c`](../../../../../libfreerdp/core/rdp.c), `rdp_read_header` and `rdp_recv_tpkt_pdu`; [`transport.c`](../../../../../libfreerdp/core/transport.c), `parse_default_mode_pdu` and `transport_default_read_pdu`.

## Reproduction and scope

[results.json](results.json) contains all eight fresh case/mode executions, classifications, and hashes. [evidence/](evidence/) contains selected raw TRX, native logs, and bridge event logs. Certificates, private keys, and binaries are excluded. Every run used one actual client launch and the strict peer-disconnect verifier. Initial pre-repair false-positive passes are not treated as evidence.

From the FreeRDP repository root, with the pinned Microsoft checkout and the already prepared seven-patch suite:

```sh
python3 tools/windows-protocol-tests/investigations/2026-10-15-removed-negative-tests/slow-path/reproduce.py \
  --source /path/to/WindowsProtocolTestSuites \
  --work /tmp/wpts-slow-negative-reproduction
```

This exports fresh source, applies the seven registered patches plus [instrumentation.patch](instrumentation.patch), builds an isolated driver, and runs the original/corrected-fixture cases in both modes. It leaves production sources, prepared driver, and profile membership unchanged. Pass `--private-core /path/to/diagnostic-library-directory` to also run the MCS controls using the private macOS core built by the sibling Demand Active investigation ([build-native.py](../demand-active/build-native.py), [diagnostic-native.patch](../demand-active/diagnostic-native.patch)).

Recommendation: fix MCS length validation with compatibility/regression coverage before restoring that test; fix Microsoft's TPKT serialization mutation and qualify the repaired case before restoring it. Neither case was added back to extended by this investigation.

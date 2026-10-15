# Removed Demand Active negative tests

Both failures expose **FreeRDP protocol length-validation defects**. Microsoft generates real inconsistent lengths, and its repaired disconnect assertion correctly detects that the original client does not disconnect. The MCS generator alters a different amount than a casual reading of its decrement suggests; that does not invalidate this negative test.

## Evidence

All packets are 401 bytes, including a 15-byte TPKT/X.224/MCS envelope and a 386-byte Demand Active payload. Offsets below are zero-based. Hex captures serialize the outgoing object immediately before `SendPdu`; TLS has no internal RDP encryption state, and the send path serializes the same object again. This is a serialization capture, not a network packet capture.

| Case | TPKT length | MCS length at offsets 13–14 | Share Control length at offsets 15–16 | Physical payload |
|---|---:|---:|---:|---:|
| Positive control | 401 | `81 82` = 386 | `82 01` = 386 | 386 |
| InvalidMCSLength | 401 | `80 82` = 130 | `82 01` = 386 | 386 |
| InvalidPDULength | 401 | `81 82` = 386 | `81 01` = 385 | 386 |

The MCS test decrements byte 13, the high octet of this two-octet PER length. It subtracts **256**, not 1. Its documented requirement is a length smaller than the actual data, which the packet satisfies. The Share Control test subtracts exactly 1. The normal SDK encoder preserves that field mutation.

| Test | Original standard | Original bridge | Diagnostic repair standard | Diagnostic repair bridge |
|---|---|---|---|---|
| `S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidMCSLength` | Failed | Failed | Passed | Passed |
| `S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidPDULength` | Failed | Failed | Passed | Passed |
| `BVT_ConnectionTest_CapabilityExchange_PositiveTest` | Passed | Passed | Passed | Passed |

These are **12 actual executions**, using one client launch per case, exact-name TRX results, and the strict disconnect verifier. Original-client logs show capability initialization and subsequent termination initiated by test cleanup. Repaired-client logs explicitly reject `declared=130 actual=386` and `declared=379 actual=380` (the latter excludes the six-byte Share Control header). Both repaired negative cases pass the disconnect assertion before cleanup.

`evidence/baseline` and `evidence/diagnostic-repair` contain summary JSON, all six serialized packets per configuration, TRX results, client/driver logs, bridge events, and the Microsoft debug logs. `hashes.json` records original and diagnostic binary/source hashes without including executables, libraries, certificates, or keys.

## Root causes and primary specification

At FreeRDP revision `0ed6fd412a856a85ea94866e5695fd7dd37b9b0b`:

- `libfreerdp/core/rdp.c:550–667`, especially line 662: `rdp_read_header` reads the MCS `userData` length but only verifies that at least that many bytes remain. It does not reject extra bytes or bound the downstream stream to the declared length. Thus a declared 130-byte payload is parsed as the physically present 386-byte Demand Active payload.
- `libfreerdp/core/connection.c:1327–1361`, especially line 1353: `rdp_client_connect_demand_active` passes null for both length outputs of `rdp_read_share_control_header`. That header function (`rdp.c:249–317`) checks minimum availability, then the caller discards its declared boundary. `rdp_recv_demand_active` (`capabilities.c:4623–4671`) consumes the full stream. Its final `tpkt_ensure_stream_consumed` (`tpkt.c:135–154`) checks that no physical bytes remain, not whether the declared nested length matched.

Microsoft's [MS-RDPBCGR §3.2.5.3.13.1, Processing Demand Active PDU](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/0108279d-cd8b-4975-993c-a5cb9ee0a888) requires consistency checks for the MCS and Share Control lengths. It recommends disconnecting for discrepancies. The normative strength is **MUST examine**, **SHOULD disconnect**; the test's enabled `DropConnectionForInvalidRequest` profile selects that recommended behavior. The [Share Control Header definition](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/73d01865-2eae-407f-9b2c-87e31daac471) includes its own six bytes in `totalLength`.

At Microsoft revision `29ddb4238a5443b7499e82934c90cc898861c5e7`:

- `TestSuites/RDP/Client/src/Adapter/RDPBCGR/RdpbcgrAdapter.cs:965–986`: negative packet selection / Share Control mutation.
- `TestSuites/RDP/Client/src/Adapter/RDPBCGR/RDPBCGRTypes.cs:578–602`: `Server_Demand_Active_Pdu_Ex.ToBytes`, including `rawData[13]--`.
- `ProtoSDK/MS-RDPBCGR/Server/RdpbcgrServer.cs:1646–1710`: normal Demand Active construction.
- `ProtoSDK/MS-RDPBCGR/Types.cs:4261–4278` and `4344–4368`: serializes the common envelope and preserves the Share Control header.
- `TestSuites/RDP/Client/src/TestSuite/RDPBCGR/S1_ConnectionTest_CapabilityExchange.cs:127–219` and `340–434`: connection setup and disconnect assertions.

## Diagnostic repair and limits

`diagnostic-native.patch` adds an MCS payload equality check and a Demand Active-specific Share Control remaining-length equality check. `build-native.py` compiles only those two changed copies in a private directory and relinks the existing core objects into a private library. The original source and build remain untouched. `DYLD_LIBRARY_PATH` selects the private library for diagnostic execution; explicit diagnostic rejection messages confirm that it was loaded.

This proves the missing checks explain these failures. It is **not a production-qualified fix**: no full extended-suite run, external server compatibility, legacy security, batching, or reconnect qualification was performed for this diagnostic repair. Production implementation should review the full packet framing and decryption paths and add focused native regressions before restoring these tests to extended.

No crash, memory corruption, buffer-allocation overread, or exploit was demonstrated. The client consumes bytes beyond declared inner protocol boundaries while remaining inside the physically received packet. The observed issue is acceptance of malformed protocol framing, followed by capability processing rather than rejection.

## Reproduction

Use the FreeRDP worktree at the recorded revision, the existing protocol-test native build, and the prepared seven-patch Microsoft suite. `build-adapter.py` copies the pinned Microsoft adapter sources into a private output directory, applies the included capture/strict-assertion diagnostic patch, and builds against the prepared suite assemblies. It deliberately only reproduces the adapter changes needed by these three scenarios; it is not a replacement prepared suite for other tests.

```sh
python3 build-adapter.py /path/to/pinned/WindowsProtocolTestSuites /path/to/FreeRDP /tmp/demand-adapter
python3 build-native.py /path/to/FreeRDP /tmp/demand-native
PROBE_ADAPTER=/tmp/demand-adapter/bin/RDP_ClientAdapter.dll python3 probe.py /tmp/demand-baseline
DYLD_LIBRARY_PATH=/tmp/demand-native PROBE_ADAPTER=/tmp/demand-adapter/bin/RDP_ClientAdapter.dll python3 probe.py /tmp/demand-repaired
```

Run `probe.py` from this tracked directory so that its relative harness lookup resolves. All output directories must be new. Live probes bind loopback sockets and launch the SDL client. `PROBE_CLIENT` optionally selects another client binary. Private run directories contain temporary credentials/certificates; copy only the reviewed evidence files when preserving results.

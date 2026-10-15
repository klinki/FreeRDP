# Restored negative tests — production repair and qualification

All five investigated tests are restored to **extended**, bringing it to **120
cases**. Quick remains **6**. The production client and final eight-patch
Microsoft driver passed a complete 120-case standard run and a complete 120-case
serial bridge run: **240 selected executions passed**.

| Restored case | Repair |
| --- | --- |
| Demand Active `InvalidMCSLength` | MCS payload length must match the remaining enclosing TPKT body. |
| Demand Active `InvalidPDULength` | Demand Active must fit its declared Share Control boundary after decryption. |
| Slow-path output `InvalidMCSLength` | Uses the same MCS consistency check as Demand Active. |
| MCS disconnect `InvalidTPKTLength` | Microsoft fixture now declares 8 bytes for its 9-byte packet. |
| Slow-path output `InvalidTPKTLength` | Microsoft fixture now declares 39 bytes for its 40-byte TLS packet; serialization happens once. |

The exact method names, options, patch hashes, client/library hashes and all
results are in [validation-restored.json](../../validation-restored.json).
The [prior investigation](../2026-10-15-removed-negative-tests/README.md) is retained
as evidence of the original failures and isolated controls.

## Native implementation and compatibility

[rdp.c](../../../../libfreerdp/core/rdp.c) captures the TPKT length and stream
starting position before decoding the MCS header. It compares the decoded MCS
userData length with that frame's remaining body, before security headers or
FIPS padding are removed. It does not confuse buffer suffix bytes with data in
the current TPKT. Multiple Share Control PDUs can still occupy one MCS payload.

[connection.c](../../../../libfreerdp/core/connection.c) retains the Share Control
remaining length and checks it for Demand Active after decryption. The check is
scoped to Demand Active; ordinary batching, flow-control PDUs and short
Deactivate All handling remain available.

[TestRdpPacketLengths](../../../../libfreerdp/core/test/TestRdpPacketLengths.c)
executes 84 scenarios through production parser functions: client/server MCS,
one/two-octet PER lengths, embedded-buffer offsets and suffixes, inconsistent
lengths, TLS/basic security, actual RC4/FIPS encryption/decryption, FIPS padding,
batched Share PDUs, short Deactivate All, and out-of-sequence flow control.
All eight core CTests passed. The new test also failed against each isolated
original parser object and both originals together, demonstrating regression
detection. [Native evidence](native/) includes logs and exact source/library
hashes; no original or repaired binaries are committed.

The qualification build enables `BUILD_TESTING_INTERNAL=ON` to expose parser
symbols for native tests, with `BUILD_TESTING=OFF` as required by this repository's
internal-test configuration. Protocol/codec options remain unchanged, including
`WITH_GFX_AV1=OFF` and `WITH_GFX_AZURE=OFF`. Commands used:

```sh
cmake -S . -B build/protocol-tests -DBUILD_TESTING=OFF -DBUILD_TESTING_INTERNAL=ON
cmake --build build/protocol-tests --target sdl3-freerdp TestCore -j2
ctest --test-dir build/protocol-tests/libfreerdp/core/test --output-on-failure
```

## Microsoft fixture implementation

The eighth registered [patch](../../patches/tpkt-short-length-fixtures.patch)
fixes the zero-length sentinel underflow in both adapters. Disconnect measures
its unencrypted encoded size before setting the short network-order length.
Slow-path uses a wrapper that serializes once and changes only the resulting
TPKT length bytes. This avoids advancing Standard RDP encryption state twice.

Nine [actual-adapter regressions](fixtures/README.md) passed against the final
driver, including valid/unrelated packet controls and real 128-bit encryption.
The encrypted pointer frame remains 52 bytes and declares 51; cipher state advances
once and its ciphertext/MAC match the positive control. The earlier driver
fails four targeted checks, while its five unrelated controls pass. No Microsoft
test method or strict disconnect assertion was weakened.

## Live qualification

The five restored cases first passed standard and bridge modes (10 executions).
The complete extended profile was then run in both modes. An initial parallel
bridge attempt had a driver startup/accept failure in the existing desktop-
composition capability case, before any TCP/RDP establishment: Microsoft queued
an asynchronous listener exception, then the client received connection refused.
Microsoft's adapter discarded the underlying exception details. A race around
the harness's released ephemeral port is plausible but unconfirmed.

That failed attempt is retained in the qualification record and
[evidence/](evidence/), rather than counted as a pass. A subsequent complete
serial bridge run passed all 120 cases. The standard run also passed all 120.
The final 240 selected case/mode results therefore come from two complete passing
runs on the same native libraries and eight-patch driver. Native hashes were
checked before and after qualification. The normal `--mode both` runner executes
modes serially.

All 35 Python harness/catalog checks passed, and the generated 432-method catalog
matches the final profile. Full artifacts remain in ignored local result
directories; the restored cases' TRX, client/driver/PTF logs and bridge events are
retained here with [hashes](evidence-sha256.json). Certificates and private keys
are excluded.

This is macOS arm64 live qualification using TLS. Native regression coverage of
legacy decryption does not add a legacy-security live profile. Graphics still
checks protocol/frame acknowledgements rather than pixel accuracy, and the
other exclusions remain documented in the [catalog](../../test-catalog.html).

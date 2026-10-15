# TPKT fixture repair and regression — 2026-10-15

The pinned Microsoft adapter decremented an unset TPKT length in both
`S1_ConnectionTest_Disconnection_NegativeTest_MCSDisconnectProviderUltimatum_InvalidTPKTLength`
and `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidTPKTLength`.
Zero is the SDK's automatic-length placeholder, so the subtraction produced
65,535 instead of the intended encoded packet size minus one.

The registered [fixture patch](../../../patches/tpkt-short-length-fixtures.patch)
repairs the generators without changing the Microsoft test methods or their
strict peer-disconnect assertion:

- The disconnect generator measures its encoded size before assigning the
  undersized network-order field. Its original packet type is preserved, so the
  SDK still records the disconnect reason.
- The slow-path generator uses a wrapper that serializes once and changes only
  the two serialized TPKT length bytes. This also preserves Standard RDP
  encryption state; measuring and serializing an encrypted packet twice would
  advance its cipher state twice.

| Packet | Actual bytes | Correct positive length | Correct negative length |
| --- | ---: | ---: | ---: |
| MCS disconnect, either reason | 9 | 9 | 8 |
| TLS slow-path pointer position | 40 | 40 | 39 |
| Encrypted slow-path pointer position | 52 | 52 | 51 |

## Regression coverage

[Program.cs](Program.cs) invokes the actual prepared `RdpbcgrAdapter` entry
points and captures the SDK's real send output in memory. A local ephemeral TCP
socket pair supplies the endpoint used by the SDK lookup; no RDP client,
external server, TLS handshake, or receive thread is involved. The test does not
copy the repaired serialization code.

Nine checks cover both disconnect reasons, positive and malformed TLS frames,
unchanged MCS and X.224 mutations, and the encrypted slow-path case. The last
check initializes two independent sessions with deterministic 128-bit Standard
RDP Security keys. Both sessions must advance their encryption count exactly
once, and their complete emitted frames must match except for the TPKT length.
This is driver compatibility coverage, not an addition of legacy encryption to
our TLS-only live profiles.

[Baseline evidence](baseline/result.json) against the previous seven-patch suite
contains five successful controls and four expected failures, each exposing the
65,535-byte underflow. [Patched evidence](patched/result.json) against the final
prepared eight-patch suite contains nine passes. Each result includes assembly
and source hashes, SDK version, raw packet hex, and build/run logs. End-to-end
standard/bridge qualification is recorded in the parent restoration report.

From the FreeRDP repository root, after preparing the patched Microsoft suite:

```sh
python3 tools/windows-protocol-tests/investigations/2026-10-15-restored-negative-tests/fixtures/run.py \
  --suite-bin /path/to/prepared-suite/Bin \
  --output /tmp/wpts-tpkt-fixture-regression
```

The helper requires .NET SDK 8, allows only a new output directory, and builds in
a temporary directory. Its process needs permission to bind a loopback socket.
The patch applies after the seven earlier repairs to Microsoft revision
`29ddb4238a5443b7499e82934c90cc898861c5e7`; its SHA-256 is
`35d6ab19ff76a524a1c0ca9e001910ea2f082c29c1c9eac9a448ff056365ecf1`.

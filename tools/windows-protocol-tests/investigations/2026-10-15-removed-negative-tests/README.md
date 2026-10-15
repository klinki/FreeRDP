# Five removed negative tests — findings

> Follow-up: the [production repairs and restoration](../2026-10-15-restored-negative-tests/README.md) are complete. Extended now has 120 cases; this document preserves the earlier investigation and qualification state.

The suite additions were committed as `0ed6fd412` on `codex/windows-protocol-smoke`.
This follow-up investigated all five tests removed when the disconnect verifier
was corrected. **Three tests expose two FreeRDP length-validation defects. Two
tests expose a Microsoft TPKT-fixture underflow.** All five are useful scenarios;
none should be dismissed as inapplicable to TLS.

| Microsoft test | Finding | Captured inconsistency | Details |
| --- | --- | --- | --- |
| `S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidMCSLength` | FreeRDP validation defect | MCS userData length: declares 130, carries 386 bytes. | [Evidence](demand-active/README.md) |
| `S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidPDULength` | FreeRDP validation defect | Share Control totalLength: declares 385, carries 386 bytes. | [Evidence](demand-active/README.md) |
| `S1_ConnectionTest_Disconnection_NegativeTest_MCSDisconnectProviderUltimatum_InvalidTPKTLength` | Microsoft fixture defect | TPKT packet length: declares 65,535, carries 9 bytes. | [Evidence](disconnect/investigation.md) |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidMCSLength` | FreeRDP validation defect | MCS userData length: declares 25, carries 26 bytes. | [Evidence](slow-path/README.md) |
| `S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidTPKTLength` | Microsoft fixture defect | TPKT packet length: declares 65,535, carries 40 bytes. | [Evidence](slow-path/README.md) |

## FreeRDP findings

The two MCS cases share one cause: `rdp_read_header` in
[rdp.c](../../../../libfreerdp/core/rdp.c) checks minimum payload availability but
does not enforce the declared inner boundary. The Demand Active test therefore
parses 386 bytes after an MCS length of 130; the slow-path pointer update parses 26
after a declared 25. The Demand Active fixture decrements the high PER length
octet, subtracting 256 rather than 1; it still creates the documented smaller
length and remains a valid negative test.

The remaining Demand Active case has a separate cause:
`rdp_client_connect_demand_active` in
[connection.c](../../../../libfreerdp/core/connection.c) discards the Share Control
length outputs and passes the entire received stream to capability parsing.
The header declares 385 bytes while 386 are parsed.

Microsoft requires examining these lengths for consistency and recommends
connection closure on discrepancies: [Demand Active processing §3.2.5.3.13.1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/0108279d-cd8b-4975-993c-a5cb9ee0a888)
and [server-to-client slow-path processing §3.2.5.2](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/dac03011-f059-44d3-a30b-26f238ad21aa).
The distinction is **MUST check consistency**, **SHOULD disconnect**. These
findings concern the missing boundary validation, not merely a policy preference
about disconnecting.

A private native library containing the two missing checks makes all three
unchanged negative tests pass in both modes. The valid capability-exchange
control passes before and after in both modes. That establishes causality;
the small diagnostic repair still needs production compatibility and native
regression coverage. The observed reads stay within the physically received
packet; this investigation demonstrated no crash, memory corruption or exploit.

## Microsoft fixture findings

Both TPKT generators decrement a zero-valued unsigned length before serialization.
Zero is the SDK's sentinel for filling the actual size; after underflow to 65,535,
the encoder preserves that value. The disconnect fixture sends 9 bytes; the
slow-path fixture sends 40. Their plans describe a length **smaller** than actual. The corrected fixtures
use actual length minus one: 8 and 39 respectively.

The stock FreeRDP client passes the unchanged strict assertions in both modes
when those two fixtures emit the intended lengths. The disconnect case was also
checked with valid frames, deliberately incomplete frames, delayed completion,
fragmentation and EOF controls. These confirm that the original timeout is
waiting for a declared incomplete frame, not acceptance of a malformed complete
PDU. TCP and TLS do not preserve application message boundaries; a single write
or TLS record cannot establish TPKT completion. See [RFC1006 §6](https://www.rfc-editor.org/rfc/rfc1006.html#section-6)
and [RFC5246 §6.2.1](https://www.rfc-editor.org/rfc/rfc5246.html#section-6.2.1).

EOF controls deliberately close the server stream and produce driver disposal
exceptions; their client network-disconnect logs are diagnostic evidence only.
They are not counted as passing original tests, and bridge cleanup cancellation
is not treated as autonomous client termination.

## Evidence and recommended repairs

The investigation includes 40 targeted case/mode executions: 12 Demand Active,
8 slow-path and 20 disconnect. Baseline failures and negative/EOF controls are
preserved with their meanings; 40 is not a pass count. Exact serialized/write
bytes, TRX results, logs, hashes, diagnostic patches and reproduction helpers
are linked above and indexed in [results.json](results.json).

Production sources and the prepared suite remain at the committed baseline;
quick still has 6 cases and extended 115. The diagnostic patches are stored only
under this investigation and are not applied by `prepare.py`.

Recommended follow-up:

1. Implement and regression-test MCS payload-boundary validation, including
   normal traffic, security/decryption and framing compatibility; restore the
   two original MCS tests after qualification.
2. Implement and regression-test Demand Active Share Control boundary validation;
   restore its original negative case after qualification.
3. Correct both Microsoft TPKT fixtures to derive their size from serialized
   bytes before subtracting one; qualify and restore those two cases.

The earlier five removals were justified because their old apparent success
relied on a faulty disconnect verifier. The new findings distinguish valid
client-defect tests from invalid fixture construction; they do not undo that
verifier repair or weaken the assertions.

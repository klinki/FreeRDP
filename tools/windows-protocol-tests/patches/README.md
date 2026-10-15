# Pinned Microsoft driver repairs

These patches apply to the Microsoft revision in `../suite.json`, not FreeRDP
production code. `prepare.py` exports that revision into a temporary source
folder, checks each patch digest, applies it, and builds with .NET 8. The original
checkout remains untouched. Prepared-suite provenance contains the ordered
patch paths and hashes; `run.py` rejects a stale or unpatched suite.

| Patch | Purpose |
| --- | --- |
| `drive-static-channel-prerequisites.patch` | Literal NUL channel lookup, version-correct drive capability lengths, and consistent user-logon negotiation. |
| `driver-disconnect-verification.patch` | Require an actual disconnect event; ordinary packets and unrelated exceptions cannot count as a successful negative test. |
| `session-graceful-disconnect.patch` | Accept the protocol's legal immediate MCS user-requested disconnect sequence, requiring the correct reason and actual peer closure. |
| `session-reconnect-observation.patch` | Require the client to remain connected during the bounded interval after a reconnect-failure status. |
| `udp-piggyback-ack-fixture.patch` | Require a real piggyback ACK derived from observed peer data in the specifically named DATA-with-ACK scenario. Apply after the RDPEUDP2 repair. |
| `udp-rdpeudp2-driver.patch` | Repair optional ACK handling, ACK-vector offsets without timestamps, and cumulative ACK matching across sequence wraparound. |
| `udp-listener-shutdown.patch` | Wake the blocking local UDP receive before closing the listener on macOS. |
| `tpkt-short-length-fixtures.patch` | Repair the disconnect and slow-path zero-length underflow; serialize encrypted slow-path packets once before changing their TPKT length. |

The [restoration report](../investigations/2026-10-15-restored-negative-tests/README.md)
records final native/fixture repairs and full two-mode qualification.
The [batch investigation](../investigations/2026-10-15-extended-batches/README.md)
records the individual qualifications, negative controls, and remaining limits.
Tests retain their original protocol scenarios; added checks strengthen missing
observations. No graphics assertion is relaxed to accept an ignored packet or
channel error when the test requires connection closure.

Protocol references:

- [MS-RDPBCGR client-initiated disconnect](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/27915739-8f77-487e-9927-55008af7fd68)
- [MS-RDPEUDP2 cumulative ACK semantics](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/32be8113-cdb4-4843-809c-3fa3aa886971)

Microsoft's source and these derived modifications are under the MIT license;
preparation copies the upstream license into `MICROSOFT-LICENSE.txt`. The suite
source and binaries are not vendored. To revise a patch, update its SHA-256 in
`suite.json`, rebuild to a new output directory, requalify affected cases in both
launch modes, and regenerate the catalog.

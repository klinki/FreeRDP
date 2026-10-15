# Microsoft RDP protocol tests

Run selected tests from Microsoft's **RDP Client Endpoint Test Suite** against
the local FreeRDP SDL3 executable. The Microsoft driver acts as the RDP server;
the qualified macOS/TLS cases need no Windows VM or remote desktop account.
The client opens short-lived windows in the current graphical session.

Browse the [interactive catalog](test-catalog.html) for all **432** Microsoft RDP
Client tests. It filters by Area, Test Suite and Implementation status, with a
short description, pinned source link and explanation for every excluded case.
“Implemented” means integrated into this runner, not complete feature conformance.

| Profile | Cases | Purpose |
| --- | ---: | --- |
| `quick` (default) | 6 | Connection setup, disconnect and malformed negotiation responses |
| `extended` | 120 | Includes quick, connection/capability checks, graphics, static drive channels, RDPEUDP2 and session recovery |

Launch mode is independent: `--mode standard`, `bridge`, or `both`. Both modes
produce 12 executions for quick and 240 for extended. The current
[qualification record](validation-parallel.json) contains **480/480 passing
executions** on the final eleven-patch driver: four concurrent full extended runs,
two standard and two bridge. The [parallel investigation](investigations/2026-10-15-parallel-listener/README.md)
records listener readiness, cleanup and event-ordering repairs, with startup
metadata and a simultaneous four-listener socket snapshot. Earlier failed
attempts remain preserved separately. The six quick cases passed in every worker.

The [restoration report](investigations/2026-10-15-restored-negative-tests/README.md)
records the production fixes, native regressions and all five restored cases.
The [earlier batches](investigations/2026-10-15-extended-batches/README.md) and
[root-cause investigation](investigations/2026-10-15-removed-negative-tests/README.md)
remain historical evidence. The original [92-case qualification](validation-profiles.json)
used the old disconnect verifier and cannot establish the removed cases' validity.
Quick remains unchanged:

| Case | Coverage |
| --- | --- |
| `BVT_ConnectionTest_ConnectionInitiation_PositiveTest` | X.224 negotiation and TLS transition |
| `BVT_ConnectionTest_BasicSettingExchange_PositiveTest_ExtendedClientDataSupported` | Extended client settings exchange |
| `BVT_ConnectionTest_CapabilityExchange_PositiveTest` | Client/server capability exchange |
| `BVT_ConnectionTest_Disconnection_PositiveTest_ServerInitiated` | Connected session and server-initiated disconnect |
| `S1_ConnectionTest_ConnectionInitiation_NegativeTest_RDPNegotiationFailure` | Rejection of failed negotiation |
| `S1_ConnectionTest_ConnectionInitiation_NegativeTest_InvalidTPKTHeader` | Disconnect after a malformed connection response |

## Per-case settings and coverage

[`suite.json`](suite.json) pins the source revision, ordered patch hashes,
profile membership and case options. Extended includes quick by reference.
Settings reset between cases:

| Cases | Configuration / observation |
| --- | --- |
| Extra-large pointer capabilities | Microsoft RDP version 10.8 |
| Continuous network detection and five static-channel cases | `SupportRDPEFS=true`; isolated empty redirected folder |
| Video-channel support | `/video` |
| Graphics capability, compression, segmentation and H.264 cases | Fullscreen startup; `/gfx:AVC444,mask:0x200`; protocol/frame-ACK checks |
| Both auto-reconnect cases | Opt-in reconnect; interrupt one owned loopback TCP relay connection; require one client launch and two connections |
| Client-initiated disconnect | Owned-process SDL signal handler or bridge cancel; require actual MCS user-requested disconnect plus peer closure |

The graphics mask filters FreeRDP's legacy 10.6 errata alias `0x000A0601`, which
the pinned Microsoft validator rejects. These cases require `/buildconfig` to
report `WITH_GFX_AV1=OFF` and `WITH_GFX_AZURE=OFF`; other layouts fail explicitly.
This is a test-only compatibility setting, not an engine default change.
**Frame acknowledgements do not prove pixel accuracy.** Positive drive cases
exercise RDPEFS announcement/capability/device-list exchange, not file operations
or explicit chunk-boundary/compression-flag correctness.

## Prepare

Requirements: Python 3.11+, stable .NET 8 SDK, Git, `tar`, OpenSSL supporting
`req -addext`, and a built SDL3 client. Preparation can access GitHub/NuGet;
subsequent test runs use the local build. Python uses only its standard library.

```sh
python3 tools/windows-protocol-tests/prepare.py
```

Preparation exports the pinned Git objects into a temporary source directory,
applies the [reviewed driver repairs](patches/README.md), and publishes the RDP
client test assembly into `.cache/suite`. It copies test data and the Microsoft
MIT license. The input checkout is not patched; source and binaries are not
vendored. `global.json` selects .NET 8, while `build.log` and `provenance.json`
record the build and exact ordered patch hashes.

Existing output, wrong source revisions, tracked source modifications and
changed patch bytes are rejected. Choose a new output directory when rebuilding:

```sh
python3 tools/windows-protocol-tests/prepare.py \
  --source /path/to/WindowsProtocolTestSuites \
  --output /path/to/new-suite
```

A prepared suite from before these repairs is rejected by the runner. Rebuild
and pass `--suite /path/to/new-suite`. The driver repairs fix malformed drive
prerequisites, false disconnect detection, RDPEUDP2 ACK handling/shutdown, the
required piggyback fixture, missing reconnect observation, short-TPKT fixtures,
listener readiness and racing retry-connection events during disconnect checks. Graceful close
accepts the alternative sequence explicitly permitted by MS-RDPBCGR. None
changes production FreeRDP behavior or relaxes a required disconnect assertion.

Build SDL3 with `WITH_CLIENT_SDL=ON`, `WITH_CLIENT_SDL3=ON` and, for bridge mode,
`WITH_LAUNCHER_BRIDGE=ON`; see the [bridge documentation](../../client/SDL/SDL3/launcher-bridge.md).

## Run

```sh
# Default quick profile.
python3 tools/windows-protocol-tests/run.py \
  --client build/protocol-tests/client/SDL/SDL3/sdl-freerdp --mode both

# Complete extended profile.
python3 tools/windows-protocol-tests/run.py \
  --client build/protocol-tests/client/SDL/SDL3/sdl-freerdp \
  --profile extended --mode both

# Inspect selection without a client, suite build or network access.
python3 tools/windows-protocol-tests/run.py --list
python3 tools/windows-protocol-tests/run.py --profile extended --list
```

Use repeatable `--case TEST_NAME` to select a batch from the chosen profile;
unknown names and duplicates fail. `--suite` selects another prepared build and
`--output` requires a new results directory. Default driver deadline is 90 seconds;
`--timeout` accepts 10–600 seconds, followed by bounded cleanup.

`standard` launches SDL directly; `bridge` uses its inherited private socket and
authentication/certificate messages. It does not run the Swift launcher UI.
`both` executes modes serially. To run concurrently, launch independent runner
processes, each with its own results directory (the default creates a unique
private directory atomically). Each run copies its prepared suite so configuration
and PTF logs do not alter another run. Results include `summary.json`, provenance,
exact-name TRX, driver/client/PTF logs, bridge events and per-case `startup.json`.
Startup diagnostics retain listener/control endpoints, process IDs, timestamps,
driver exit status and adapter exception details. Reconnect/close action
counts are recorded. A pass requires an actual matching TRX Passed result,
successful driver exit, one client launch and no adapter/bridge errors. Reconnect
requires the intended interruption and second connection; bridge mode also
checks reconnect state transitions. Skips, no-test passes and timeouts fail.

## Environment and limits

The Microsoft RDP listener, control adapter and reconnect relay bind only IPv4
loopback in this runner. The real Microsoft listener selects and retains an
OS-assigned port, then atomically publishes readiness after bind/listen succeed.
The client starts only after validating the case token, endpoint and owned driver
process group; no TCP readiness probe consumes a connection. This prevents stale
or cross-case handoffs, but does not isolate processes running as the same OS user.
The adapter accepts only the published test endpoint and never executes
driver-supplied command strings or changes host interfaces/routes.

Credentials are synthetic `wpts` / `wpts`. Each run generates a one-day TLS
certificate, pinned by SHA-256 in standard mode and accepted only once for the
matching bridge connection. Config/cache files are isolated in private result
directories. Certificates are not installed in a system trust store. Cleanup
only targets owned child processes. Abrupt-exit controls fail graceful-close
validation; process cleanup is not accepted as protocol evidence.

This remains an opt-in macOS Apple Silicon qualification, not a release gate,
interoperability certificate or visual comparison suite. NLA needs a separate
Windows Microsoft driver. Input injection and display/topology changes need
fixtures unavailable here. Heartbeat/network-sync recovery, several malformed
packet cases and separated-chroma AVC444 failed qualification. Legacy Standard
RDP Security and v1/v2 UDP scenarios remain excluded from this TLS/v3-only profile.
The catalog records individual reasons; unsupported control commands fail.

## Catalog and regression checks

The HTML is self-contained and works offline. Regenerate after manifest or
classification changes:

```sh
python3 tools/windows-protocol-tests/generate-catalog.py --source /path/to/WindowsProtocolTestSuites
python3 tools/windows-protocol-tests/generate-catalog.py --source /path/to/WindowsProtocolTestSuites --check
python3 -m unittest discover -s tools/windows-protocol-tests -p 'test_*.py' -v
```

The generator reads pinned Git objects and checks the 432-method inventory,
profile membership, duplicates and explanations. The harness checks additionally
require passing two-mode evidence for every enabled case and its actual options.
[Disconnect SDK regressions](investigations/2026-10-15-extended-batches/drive/regression/README.md)
and [UDP regressions](investigations/2026-10-15-extended-batches/udp/investigation.md)
exercise the repaired C# implementations independently. The
[original investigation](investigations/2026-10-15-drive-udp/investigation.md)
is retained as historical evidence.

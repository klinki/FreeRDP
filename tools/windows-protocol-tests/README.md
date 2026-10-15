# Microsoft RDP protocol tests

Run selected tests from Microsoft's **RDP Client Endpoint Test Suite** against the
local FreeRDP SDL3 executable. The Microsoft driver acts as the RDP server; no
Windows VM, remote desktop account, or Sheltie checkout is needed. The client
opens short-lived windows in the current graphical session.

Browse the [interactive Microsoft test catalog](test-catalog.html) for all 432
RDP Client tests, grouped by area and split into Implemented / Not implemented.
The offline page filters by Area, Test Suite and Implementation status, with a
short description, pinned source link, and integration/applicability explanation
for every case. Here, implemented means included in our runner manifest.

Choose one of two profiles; `quick` is the default:

| Profile | Cases | Purpose |
| --- | ---: | --- |
| `quick` | 6 | Connection setup, server disconnect and malformed connection responses |
| `extended` | 92 | Includes quick, 80 additional connection/protocol cases, and six cases with audited settings |

Launch mode is independent: `--mode standard`, `bridge`, or `both`. Both modes
produce 12 executions for quick and 184 for extended. On the qualification Mac,
quick took under a minute in both modes; extended takes approximately 13 minutes.
Timings depend on the machine and failures can take longer.

[`suite.json`](suite.json) pins the Microsoft revision and lists all cases.
Extended includes quick by reference, so a case is never duplicated. The six
quick cases are:

| Case | Coverage |
| --- | --- |
| `BVT_ConnectionTest_ConnectionInitiation_PositiveTest` | X.224 negotiation and TLS transition |
| `BVT_ConnectionTest_BasicSettingExchange_PositiveTest_ExtendedClientDataSupported` | Extended client settings exchange |
| `BVT_ConnectionTest_CapabilityExchange_PositiveTest` | Client/server capability exchange |
| `BVT_ConnectionTest_Disconnection_PositiveTest_ServerInitiated` | Connected session and server-initiated disconnect |
| `S1_ConnectionTest_ConnectionInitiation_NegativeTest_RDPNegotiationFailure` | Rejection of failed negotiation |
| `S1_ConnectionTest_ConnectionInitiation_NegativeTest_InvalidTPKTHeader` | Disconnect after a malformed connection response |

The 80 additional baseline cases cover capability exchange (35), basic settings
(17), channel setup (8), finalization/reactivation (7), negotiation/licensing/
disconnect/malformed input (11), and connection-time network detection (2).

Six extended cases have settings applied only for that case:

| Cases | Configuration |
| --- | --- |
| Extra-large pointer capabilities | Test-suite RDP version 10.8 |
| Continuous RTT and bandwidth detection | `SupportRDPEFS=true` and a new empty redirected folder under that case's results |
| Video-channel protocol support | `/video` |
| Graphics capability exchange and H.264 AVC420 baseline | Honor fullscreen requests; `/gfx:AVC444,mask:0x200` |

The graphics mask removes only FreeRDP's legacy 10.6 errata alias `0x000A0601`,
which the pinned Microsoft validator rejects. It is confined to these two
compatibility tests, not a change to the engine's defaults. Mask bit positions
depend on build options: these cases require `/buildconfig` to explicitly report
`WITH_GFX_AV1=OFF` and `WITH_GFX_AZURE=OFF`. Unknown or different configurations
fail before running; use a supported build or select non-graphics cases.
The remaining cases keep the normal capability advertisement. The graphics
tests check protocol responses and frame acknowledgements, not pixel accuracy.

## Catalog maintenance and investigation

The catalog is a self-contained HTML document; open `test-catalog.html` directly
in a browser. It needs no server or network connection for filtering. Source and
evidence links are available when their targets can be opened.

Regenerate it from the pinned Microsoft checkout after changing `suite.json` or
the reviewed rules in `catalog-config.json`:

```sh
python3 tools/windows-protocol-tests/generate-catalog.py \
  --source /path/to/WindowsProtocolTestSuites
python3 tools/windows-protocol-tests/generate-catalog.py \
  --source /path/to/WindowsProtocolTestSuites --check
```

The generator reads the pinned Git objects, so local diagnostic patches cannot
silently change the inventory. It checks unique names, profile membership,
revision/inventory drift, and a reason for every unassigned case. Current source
inventory and compiled test discovery both contain exactly 432 methods. The
HTML separates integration status from dated qualification evidence.

The [drive-channel and UDP investigation](investigations/2026-10-15-drive-udp/investigation.md)
records the original failures, targeted reproductions, and isolated Microsoft
compatibility patches. The patches are evidence and are **not applied** by
`prepare.py`. No extra tests were enabled from that investigation.

## Prepare

Requirements: Python 3.11+, the .NET 8 SDK, Git, OpenSSL with `req -addext`
support, and a built SDL3 client. Preparation needs network access to GitHub and
NuGet; subsequent runs use the local build. Python uses only its standard library.

From the FreeRDP repository root:

```sh
python3 tools/windows-protocol-tests/prepare.py
```

This clones the pinned Microsoft source into `.cache/source`, publishes only the
RDP client test project into `.cache/suite`, and copies its test data and license.
The Microsoft source and dependencies are not vendored into FreeRDP. A local
`global.json` selects .NET 8 without changing the machine's default SDK.
`build.log` and `provenance.json` record the build. Existing output directories,
wrong source revisions, and tracked modifications to the Microsoft source are
rejected. To use an existing clean checkout or retain another build:

```sh
python3 tools/windows-protocol-tests/prepare.py \
  --source /path/to/WindowsProtocolTestSuites \
  --output /path/to/new-suite-directory
```

Build the SDL client using the normal FreeRDP build instructions, with
`WITH_CLIENT_SDL=ON` and `WITH_CLIENT_SDL3=ON`. Bridge mode additionally requires
`WITH_LAUNCHER_BRIDGE=ON`; see the [bridge documentation](../../client/SDL/SDL3/launcher-bridge.md).

## Run

The default quick profile:

```sh
python3 tools/windows-protocol-tests/run.py \
  --client build/launcher/client/SDL/SDL3/sdl-freerdp \
  --mode both
```

The complete extended profile:

```sh
python3 tools/windows-protocol-tests/run.py \
  --client build/launcher/client/SDL/SDL3/sdl-freerdp \
  --profile extended --mode both
```

Inspect a profile without a client, prepared Microsoft suite, or network access:

```sh
python3 tools/windows-protocol-tests/run.py --list
python3 tools/windows-protocol-tests/run.py --profile extended --list
```

`standard` (the default) launches the SDL binary directly. `bridge` starts it
through its private inherited socket and responds to authentication/certificate
requests. `both` runs the selected profile in each mode, serially. Bridge mode also
requires a connected state and exactly one terminal event with `hadConnected`
for the server-disconnect case. It does not run the Swift launcher UI.

To select individual cases, use `--case` (repeatable). Cases must belong to the
selected profile; duplicates and unknown names are errors. Extended-only cases
require `--profile extended`. `--suite` selects another prepared suite directory;
`--output` selects a **new** results directory. Each driver invocation has a
90-second deadline, configurable with `--timeout` from 10 to 600 seconds, followed
by bounded cleanup.

```sh
python3 tools/windows-protocol-tests/run.py \
  --client /path/to/sdl-freerdp --mode bridge \
  --case BVT_ConnectionTest_ConnectionInitiation_PositiveTest \
  --output /path/to/new-results-directory
```

Every run copies the suite into its output directory, so configuration changes
and PTF logs do not alter the prepared build or another run. RDP version, drive
support and client arguments reset between cases. Results include:

- `summary.json`: per-case verdict, errors, profile, mode, options and elapsed time.
- `provenance.json`: pinned Microsoft revision, SDK, executable path/version/hash,
  selected profile/cases/options, harness Git revision and working-tree status.
  Graphics runs also record the client build configuration and compatibility mask.
- Per-case `result.trx`, `driver.log`, client logs, PTF logs and bridge events.
- The generated test certificate and the configured suite used for reproduction.

A pass requires an actual TRX `Passed` result for exactly the requested test,
successful driver exit, exactly one client launch, and no adapter/bridge errors.
Missing tests, skipped/inconclusive results, malformed results, unsupported
control commands and timeouts fail the run. `run.py` returns nonzero if any case
fails. Cleanup terminates only the child processes created by the harness.

## Test environment and limits

The adapter listens only on loopback and only accepts connections to the local
test endpoint. It never executes command strings supplied by the driver. The
**upstream Microsoft RDP listener binds all IPv4 interfaces** on an automatically
selected port during each case; use a trusted test machine/network. No firewall
or network-interface settings are changed.

Credentials are the synthetic `wpts` / `wpts`, not a real account. TLS uses a new
one-day certificate per run. Standard mode pins its SHA-256 fingerprint; bridge
mode accepts only that fingerprint, for this connection only. FreeRDP config
and cache directories are isolated under the results directory. Result files
are created with owner-only permissions. The harness does not install the test
certificate into a system trust store.

These are protocol regressions, not an interoperability certificate or a visual
comparison test. NLA/CredSSP, automatic reconnect, UDP, screenshot comparison,
display changes, clipboard/audio/drive file operations and physical monitors
remain outside the allowlists. The scratch drive satisfies the continuous
network tests' prerequisite; it does not establish that file redirection works.

The two static-channel/drive checks remain blocked by Microsoft test-driver/SDK
issues. The original reliable-UDP probes negotiate legacy v1, which this fork's
v3-only transport intentionally declines. A separate RDPEUDP2 probe established
a v3 tunnel but did not complete its data-transfer test, so it is not qualified.
The catalog also identifies legacy Standard Security cases that do not apply to
the current TLS profile; some return without exercising a client. Upstream
assertions in the default prepared suite are unchanged. The adapter supports start/stop, including
fullscreen startup only for graphics cases. Unsupported control commands fail
explicitly.

The live suite is opt-in and is not a release gate. It has been exercised on an
Apple Silicon Mac with a logged-in graphical session; other platforms are not
yet validated. See [validation-profiles.json](validation-profiles.json) for profile
qualification and [validation.json](validation.json) for the original smoke run.

## Harness regression tests

```sh
python3 -m unittest discover -s tools/windows-protocol-tests -p 'test_*.py' -v
```

These require local socket access, but no .NET, FreeRDP binary, or external
network. They check profile selection/inheritance, case-specific arguments and
configuration reset, graphics preflight, fragmented/truncated control messages,
destination restrictions, unsupported commands, result validation, and cleanup
without terminating another client process.

Catalog tests also check inventory, membership, classification reasons, safe
inline data, deterministic output, and filter combinations. They do not need a
Microsoft checkout or .NET. Their JavaScript matching check uses Node when
available; browser QA separately verifies the real controls and responsive view.

References: [Microsoft suite](https://github.com/microsoft/WindowsProtocolTestSuites),
[RDP client guide](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/docs/RDP_ClientUserGuide.md),
and [SUT control protocol](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/RDPSUTControlAgent/Docs/SUT_Remote_Control_Protocol_RDP_Extension.md).

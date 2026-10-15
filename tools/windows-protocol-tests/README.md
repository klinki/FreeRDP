# Microsoft RDP protocol smoke tests

Run six selected tests from Microsoft's **RDP Client Endpoint Test Suite** against
the local FreeRDP SDL3 executable. The Microsoft driver acts as the RDP server;
no Windows VM, remote desktop account, or Sheltie checkout is needed for these
cases. The client opens short-lived windows in the current graphical session.

`suite.json` pins the upstream source revision and the exact test allowlist:

| Case | Coverage |
| --- | --- |
| `BVT_ConnectionTest_ConnectionInitiation_PositiveTest` | X.224 negotiation and TLS transition |
| `BVT_ConnectionTest_BasicSettingExchange_PositiveTest_ExtendedClientDataSupported` | Extended client settings exchange |
| `BVT_ConnectionTest_CapabilityExchange_PositiveTest` | Client/server capability exchange |
| `BVT_ConnectionTest_Disconnection_PositiveTest_ServerInitiated` | Connected session and server-initiated disconnect |
| `S1_ConnectionTest_ConnectionInitiation_NegativeTest_RDPNegotiationFailure` | Rejection of failed negotiation |
| `S1_ConnectionTest_ConnectionInitiation_NegativeTest_InvalidTPKTHeader` | Disconnect after a malformed connection response |

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

```sh
python3 tools/windows-protocol-tests/run.py \
  --client build/launcher/client/SDL/SDL3/sdl-freerdp \
  --mode both
```

`standard` (the default) launches the SDL binary directly. `bridge` starts it
through its private inherited socket and responds to authentication/certificate
requests. `both` runs the same six tests in each mode, serially. Bridge mode also
requires a connected state and exactly one terminal event with `hadConnected`
for the server-disconnect case. It does not run the Swift launcher UI.

To select one case, use `--case` (repeatable). `--suite` selects another prepared
suite directory; `--output` selects a **new** results directory. Each case has a
90-second deadline, configurable with `--timeout` from 10 to 600 seconds.

```sh
python3 tools/windows-protocol-tests/run.py \
  --client /path/to/sdl-freerdp --mode bridge \
  --case BVT_ConnectionTest_ConnectionInitiation_PositiveTest \
  --output /path/to/new-results-directory
```

Every run copies the suite into its output directory, so configuration changes
and PTF logs do not alter the prepared build or another run. Results include:

- `summary.json`: per-case verdict, errors, mode and elapsed time.
- `provenance.json`: pinned Microsoft revision, SDK, executable path/version/hash,
  selected cases, harness Git revision and working-tree status.
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

These are connection/protocol regressions, not an interoperability certificate
or a rendering test. NLA/CredSSP, automatic reconnect, UDP, graphics comparison,
display changes, clipboard/audio/drives and physical monitor testing are outside
this initial allowlist. The control adapter deliberately implements only
start/stop; extending the allowlist requires implementing any additional
control commands and testing their failure behavior. Upstream test assertions
remain unchanged, including rejection of malformed responses.

The live suite is opt-in and is not a release gate. It has been exercised on an
Apple Silicon Mac with a logged-in graphical session; other platforms are not
yet validated. See [validation.json](validation.json) for the recorded run.

## Harness regression tests

```sh
python3 -m unittest discover -s tools/windows-protocol-tests -p 'test_*.py' -v
```

These require local socket access, but no .NET, FreeRDP binary, or external
network. They check fragmented/truncated control messages, destination
restrictions, unsupported commands, result validation, configuration scoping,
and cleanup without terminating another client process.

References: [Microsoft suite](https://github.com/microsoft/WindowsProtocolTestSuites),
[RDP client guide](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/Client/docs/RDP_ClientUserGuide.md),
and [SUT control protocol](https://github.com/microsoft/WindowsProtocolTestSuites/blob/29ddb4238a5443b7499e82934c90cc898861c5e7/TestSuites/RDP/RDPSUTControlAgent/Docs/SUT_Remote_Control_Protocol_RDP_Extension.md).

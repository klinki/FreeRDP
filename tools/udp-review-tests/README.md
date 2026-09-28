# UDP review harness archive

Saved from the review session's `/tmp/udp-review-*` files on 2026-09-06.
These are historical diagnostic harnesses, not a replacement for `TestRdpeUdp`.
Several intentionally reproduce old defects and return success after printing
bad behavior. A successful runner exit means compilation/execution completed;
compare the output with the descriptions below to interpret the result.

## Run

Build the repository first, then run from any directory:

```sh
cmake --build /tmp/freerdp-build --target TestCore --parallel 4
bash tools/udp-review-tests/run.sh /tmp/freerdp-build
```

The runner uses the supplied build's generated headers and shared libraries.
It reads the OpenSSL include path from its CMake cache. Override `CC`,
`OPENSSL_INCLUDE_DIR`, or `RESULTS_DIR` as needed. This runner was verified on
macOS with the existing `/tmp/freerdp-build`; library paths assume FreeRDP's
Unix build layout. Generated binaries, compiler logs, and output go in ignored
`results/`. All six harnesses compiled and ran during archival. Their outputs
are also saved in `logs/archive-run-2026-09-06/`.

## Harnesses and provenance

| File in `harnesses/` | Purpose and expected interpretation |
| --- | --- |
| `udp-review-check.c` | Original encoder/protector cursor-contract investigation (R1). Intentionally passes the rewound encoder stream directly to protect; prints an all-zero protected packet. This does not claim the current production builder still does so. Also probes autodetect subheader framing. Calls the linked library. |
| `udp-review-poll.c` | Receive body refreshed for `444b698b3` (S1). With ready socket/TLS stubs and timeout zero, prints result=1, TLS_reads=1, payload_length=1. |
| `udp-review-migration.c` | Mapping/install/feed/response bodies from `1354c6e2a` (T1), with a connected-transport stub. No request: no migration; first fragment: no install; final fragment: 257 IDs installed; unlisted DVC 999: not migrated. Uses the linked extraction helper. |
| `udp-review-validation.c` | Handler body refreshed for `6fefaf21e` (U1). Whole-PDU streams start after the header. Cases 0/1 reject trailing bytes with error 13, cases 2/3 accept reliable UDP (stub 123), case 4 declines (stub 456). Uses linked strict parser. |
| `udp-review-11408-order.c` | Historical broken receive bodies from `11408bc5e` (V1/V2). Arrival 2:B,1:A prints B, and DataSeq 1,3,2 selects ACK(2). Deliberately preserves the defect. |
| `udp-review-8ddd-order.c` | Fixed receive bodies from `8ddd53a5f`: same cases print AB and ACK(3). |

The extracted function bodies do not automatically track current production
code. Minimal structs, stubbed transport/TLS functions, and copied ACK-selection
expressions make these focused reproducers, not full integration tests. Helpers
linked from the chosen build do track that build, so results mix frozen bodies
with the selected library version. Use the recorded commits when reproducing
historical behavior precisely. Earlier overwritten versions of the `/tmp`
harnesses were not available; this directory preserves every surviving C harness.

## Other preserved material

- `logs/`: nine original build logs, plus the six fresh harness outputs.
- `support/udp-review-da614-rdpeudp.c`: complete HEAD source snapshot from
  `da614a872`, saved during the separate working-directory review. Reference only;
  it is not included by the runner.
- `support/udp-review-dissector.lua`: repository Lua dissector copy with its
  protocol names changed from `rdpudp` to `reviewudp` to avoid colliding with
  Wireshark's built-in protocol, plus a Lua bit32 compatibility shim. Example:

```sh
tshark -r /path/to/capture.pcap -n \
  -X lua_script:tools/udp-review-tests/support/udp-review-dissector.lua
```

Absolute repository includes in the harnesses were replaced with repository-root
relative includes; the runner supplies that include root. No production code was
changed. Original temporary files remain in place. Platform-specific old binaries
are reproducible from source and were not copied. Traffic captures, extracted TLS
payloads, and TLS secret files are not test harnesses and are not included here.

See `../../REVIEW.md` for findings, historical validation, and live-peer limitations.

## Review of 11c4f822b

`run-11c-review.sh` runs the refreshed accept-loop socket check, extracted AOA
state diagnostic, and SVC/DVC boundary diagnostic. The localhost socket test
requires network permission. Results are in `logs/review-11c4f822b/`.

The `33e` framing harness now uses the current four-argument API: TRUE for a
CREATE response received by a server, FALSE for client-side legacy-envelope
rejection. Its original three-argument source is preserved verbatim in
`support/udp-review-33e-framing-original.c.txt`. The envelope check is now a
legacy utility rejection test, not a test of the current production sender.
The accompanying `33e` AOA harness intentionally retains its historical bug.

## Live VM connection and reconnect diagnostics

The scripts in `integration/` preserve bounded live-session diagnostics from
September 2026. They retain the historical VM, account, display, and build
defaults used for those runs. Inspect those settings before using another
environment, and supply `RDP_TEST_PASSWORD` through the environment.

- `integration/udp-connect-repeat.py` starts separate clients through a PTY,
  records connection milestones and graphics frame counts, and intentionally
  disconnects only the clients it creates. Supply `--binary` and a new local
  `--output` directory; `--count`, `--observe`, `--startup-timeout`, and `--gap`
  control the observation windows. Its historical display IDs are 3 and 2.
- `integration/reconnect-relay.py` relays TCP and UDP on localhost port 13389
  to the historical VM at `192.168.64.2:3389`, using the client at
  `/tmp/freerdp-build/client/SDL/SDL3/sdl-freerdp`. It actively aborts its TCP
  connections and drops UDP for `RDP_TEST_OUTAGE` seconds, then observes recovery.
  Configure retry count with `RDP_TEST_RETRIES` and local client-log destination
  with `RDP_TEST_LOG`. This does not simulate a silent TCP blackhole.
- [Short reconnect result](integration/reconnect-vm-result.md) and
  [retry-exhaustion result](integration/reconnect-vm-long-result.md) describe
  the original measured behavior and its limits.

The [ten-connection report](../../bugs/udp-connect-repeat-20260916.md) explains
the received-graphics readiness criterion and the initial TCP warning.
`logs/udp-connect-20260916/` retains original observations, final aggregate
analysis, compact client milestone extracts, and the exact `harness-as-run.py`.
That original runner required an ACTIVE marker without enabling its logger;
its provisional incomplete labels are not transport failures. The reusable
runner uses UDP receive migration and the first graphics frame for readiness.

Historical evidence and its measured values are unchanged. Raw client logs
and newly generated result directories remain local. The milestone extracts
are client log events, not packet captures or packet payload extracts.
No new live VM run was performed for this preservation.

## Offline September 16–17 stall diagnostics

The Python scripts under `logs/stale-20260916/` and `logs/stale-20260917/`
are historical incident-analysis fixtures. Their session paths, flow filters,
frame checkpoints, sequence offsets, and timestamps intentionally retain the
original reproduction context. They do not automatically discover new sessions.

| Script | Historical local inputs and purpose |
| --- | --- |
| `stale-20260916/check-channel-stream.py` | Reads `/tmp/freerdp-stale-20260916/channel-payloads.tsv`, reconstructs the encrypted channel stream, and checks TLS record framing around the first omitted-zero wrap. Writes the reconstructed stream locally. |
| `stale-20260916/replay.py` | Uses `tshark` and a matching FreeRDP library to replay the recorded UDP flow. `--library`, `--capture`, and optional `--expected-stream` override the stored local defaults. |
| `stale-20260916/verify-tls-continuity.py` | Reads the reconstructed stream and the original September 16 local TLS key log, then authenticates records across the sequence gap with OpenSSL 3. |
| `stale-20260917/replay.py` | Reads `/tmp/freerdp-sleep-20260917/replay-input.tsv` and the stored local FreeRDP library path, reporting stream progress at incident checkpoints. |
| `stale-20260917/verify-blocked-stream.py` | Reconstructs and authenticates the September 17 stream with its local TLS key log. Reports protocol channel counts without printing or saving application payloads. |
| `stale-20260917/analyze-stall-acks.py` | Reads `stall-udp-wire.tsv` and `inbound-sequences.tsv` under `/tmp/freerdp-sleep-20260917`, reporting ACK timing and retransmission totals. |

The replay helpers use the receive-test ABI recorded in their source; use a
matching library when reproducing historical behavior. The TLS verification
scripts use the stored Homebrew OpenSSL 3 library path. Python itself needs
only the standard library. Local capture/key-log paths remain in the scripts
as input references; the captures, keys, reconstructed payload streams, and
packet-level extracts are not included in these preservation commits.

Preserved aggregate and regression results:

- `stale-20260916/regression-before.txt`
- `stale-20260916/regression-after.txt`
- `stale-20260916/tls-continuity-result.txt`
- `stale-20260917/analyze-stall-acks.txt`
- `stale-20260917/verify-blocked-stream.txt`

These files retain the original measurements and outcomes. They are historical
results, not fresh tests of the current master. Related findings are in
[the September 16 report](../../bugs/stale-connection-20260916.md) and
[the September 17 report](../../bugs/sleep-report-20260917.md).

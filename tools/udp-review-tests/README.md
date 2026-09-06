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

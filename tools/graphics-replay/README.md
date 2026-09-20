# Offline graphics replay from packet captures

This tool extracts the **existing captured server graphics stream**, rather
than requiring a new desktop recording. Extraction opens no network socket.
The output contains the captured desktop's compressed graphics data; keep it
with the private, ignored diagnostics, not in Git.

## Extract

Requires Python 3, `tshark`, and OpenSSL `libcrypto`. On this Mac:

```sh
python3 tools/graphics-replay/extract.py /path/to/capture.pcapng \
  --keys /path/to/tls-secrets.txt --output /path/to/private/session.gfx \
  --libcrypto /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib
```

Use one complete session including the UDP ClientHello, ServerHello, and
graphics-channel creation. For a capture split across ring files, merge the
consecutive files first with Wireshark's `mergecap`. Files overwritten by the
ring cannot be recovered. This initial extractor supports one RDPEUDP2 tunnel,
TLS 1.3 AES-GCM, and ordinary DVC DATA_FIRST/DATA framing; it fails on unsupported
framing or incomplete input rather than silently dropping graphics messages.
It does not merge graphics that migrate back and forth between TCP and UDP.

The extractor deduplicates retransmissions, restores channel ordering,
authenticates every encrypted server TLS record, reconstructs tunnel and DVC
messages across fragments, and retains only the Graphics channel. An absent
channel sequence zero at a wrap is provisionally omitted only when all
remaining TLS authentication succeeds. Other gaps and conflicting retransmits
are errors. Cursor, audio, input and clipboard channels are excluded. Output
files use restrictive permissions; neither keys nor desktop payloads are logged.

`session.gfx.json` records hashes, transport validation counts, message counts,
and duration. The replay format is eight bytes `FRGFXR01`, then repeated
little-endian `uint64 timestamp_us`, `uint32 length`, and `length` bytes containing
one complete, still-ZGFX-compressed graphics message. Timestamps are relative
to the first graphics message, based on capture arrival and ordered delivery.
They are not timestamps of the original client's decoder or UI thread.

The 2026-09-20 baseline capture has been extracted into the ignored directory
`diagnostics/graphics-replay-20260920/`: 50,976 unique UDP chunks, 21 identical
retransmissions, no channel gaps, 40,049 authenticated encrypted TLS records,
and 5,192 complete graphics messages spanning 133.470 seconds.

## Extractor checks

```sh
REPLAY_LIBCRYPTO=/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib \
  python3 tools/graphics-replay/test_extract.py
```

The tests exercise reordering, duplicates, missing data, wrap labels, fragment
boundaries, channel-ID reuse, truncated input, and authenticated-decryption
failure using a public AES-GCM test vector. They contain no capture data.

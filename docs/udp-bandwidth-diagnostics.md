# UDP bandwidth-estimate diagnostics

Use these diagnostics when the RDP bandwidth estimate is implausible. They
record the existing measurement exchange; they do not run an additional speed
test, change packet accounting, correct estimates, or alter transport policy.

## Collecting a session

In Sheltie, use **Connect with Diagnostics** and select **DEBUG**. This is enough;
TRACE and TLS session keys are not required. Use the app containing the diagnostic
engine for that new connection. An already running client retains its original
code and log level. Reproduce the reading, then retain that session's `output.log`
and any rotated segments in **Open Diagnostics Folder**.

Search for `BWDIAG` under `com.freerdp.core.autodetect`. All new records are DEBUG
level and therefore absent at INFO/WARN. Existing log storage and rotation limits
still apply. There is no new file, background recorder, or per-datagram log.

## Records

| Record | Meaning |
| --- | --- |
| `start` | Received measurement start: transport, sequence, request type, and the previous timer/count before resetting that transport's measurement. |
| `payload` | Explicit measurement-payload byte count and the transport's count before adding it. This is not ordinary desktop payload logging. |
| `stop` | Received measurement stop: transport, sequence, request type, stop-payload size and byte count before adding it. |
| `result` | Result prepared for Windows: start/current monotonic milliseconds, elapsed duration, exact 32-bit duration/byte count used on the wire, and the resulting decimal kbit/s calculation. |
| `result_tx` | Result's requested measurement transport, actual send route, and whether that send succeeded. A UDP result can fall back to TCP under the existing policy. |
| `netchar_rx` | Server network-characteristics report: transport, sequence, request/header type, stream offset, field-presence flags, unsigned decimal/hex bandwidth, RTT fields and exact PDU hex. |

`result.math_valid` means both wire operands are nonzero. `derived_kbps` is
`bytes * 8 / wire_elapsed_ms`; it is zero when that calculation is undefined,
with `math_valid=0`. This diagnostic flag does not change the transmitted
response or existing telemetry acceptance rules.

`start_set`, `clock_reversed`, `duration_clamped`, and `bytes_at_limit` expose
missing/invalid timing and 32-bit limit conditions. `bytes_at_limit` reports
equality to `UINT32_MAX`; it does not establish how the count reached that value.

`netchar_rx.bandwidth_present=0` identifies an RTT-only report; its zero bandwidth
placeholder is not a new bandwidth estimate. Values labelled `bandwidth_kbps`
are decimal kilobits/s, as specified by MS-RDPBCGR. Divide by 1000 for Mbps.
RTT and all duration fields are milliseconds. Sequence numbers can wrap; correlate
records by transport, timestamps and the surrounding measurement exchange.

Diagnostic logging can affect very short measurement intervals. Use these records
to check field values and protocol behavior, rather than as an independent
throughput benchmark.

## Scope of raw data

Only a validated network-characteristics PDU is copied as hex: 14 or 18 bytes,
containing its fixed protocol header and network metrics. The copy excludes
adjacent stream data and measurement/desktop payloads. No extra tunnel-payload,
authentication or TLS-key dump is added. Formatting uses a fixed-size stack buffer
and creates no retained diagnostic history in the client.

## Reading the evidence

- Compare `netchar_rx.raw` with the decimal/hex decoded fields. A matching large
  unsigned value establishes what the parser read from this PDU, rather than a
  UI unit-conversion error.
- Compare the preceding `result` operands with the subsequent server estimate.
  Zero/short durations, maximum counts or an unexpected `result_tx` route can
  explain an invalid measurement exchange and need investigation before assigning
  the cause to the server.
- If the raw server report is sensible, follow the same value into performance
  telemetry. These diagnostics do not impose an arbitrary bandwidth ceiling.

Protocol references: [Network Characteristics Result](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/228ffc5c-b60c-4d3e-9781-ac613f822fdf)
and [server bandwidth calculation](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/b716fd9a-5a0b-4970-b5d0-f1be451408b7).

## Regression coverage

The internal-only `TestAutodetectDiagnostics` feeds synthetic PDUs through the
production parser and captures logging, callbacks and serialized responses. It
covers all three network-characteristics layouts, unsigned limit values, raw-copy
bounds, truncated messages, INFO-level silence, interleaved TCP/UDP measurement
counts, TCP fallback and zero-duration/byte-limit diagnostics. The packaged runtime
must keep `BUILD_TESTING_INTERNAL=OFF`; use a separate internal-test build.

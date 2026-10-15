# UDP/TLS callback publication race

## Observed failure and cause

The second four-worker extended cohort failed
`S3_DataTransfer_V2_ClientReceiveData` in bridge worker A, case 114.
TCP listener startup and the RDP connection succeeded. FreeRDP established
RDPEUDP2 at 15:23:34.300. At 15:23:34.456, the Microsoft driver logged a
`NullReferenceException` in `RdpeudpTLSChannel.ReceiveBytes`, line 475,
while calling `innerStream.AddReceivedData(data)`. The stack proceeds through
`RdpeudpSocket.ReceiveDataOnHigherLayer` and the RDPEUDP2 receive handler.
The exception exits the UDP server receive loop. The client's later reliable
send/TLS timeout and Microsoft's existing connection retries are consequences
of that stopped receive loop, not evidence of a FreeRDP transport defect.

The [raw driver log](live-observed/driver.log) contains the decisive exception;
use `rg -a` because the captured log contains NUL bytes. The
[client log](live-observed/client-1.log), [TRX](live-observed/result.trx), and
[startup record](live-observed/startup.json) are preserved with it.

The Microsoft TLS-channel constructor subscribes its `ReceiveBytes` callback
before allocating `innerStream`. Another thread can invoke the newly published
callback immediately, while that field is still null. There is no later
assignment setting this field to null. The observed stack identifies this
constructor publication race directly.

The [production patch](../../../../patches/udp-tls-callback-publication.patch)
initializes the inner stream and authentication flag before subscribing the
receive callback. It introduces no timeout changes, retries, dropped-data
fallbacks, or test-assertion changes. No native FreeRDP change was required.

## Deterministic scheduling proof: instrumented SDK only

The same [test-only instrumenter](instrument.py) adds one observer immediately
**after the actual event subscription** in each constructor variant. The
[regression](Program.cs) uses that observer to run the actual socket forwarder
on a separate worker thread and waits for that callback before allowing the
constructor to continue. This forces the earliest legal callback schedule;
it does not copy the constructor or ReceiveBytes implementation.

These two builds are explicitly instrumented and are **not** the production
assemblies used by the full protocol suite:

- [Baseline](baseline-instrumented/run.log): **7/9** checks pass. Both earliest
  callback cases reproduce `NullReferenceException` at the same inner-stream
  dereference, in automatic and manual transport modes.
- [Fixed](fixed-instrumented/run.log): **9/9** checks pass with identical
  instrumentation. Both early chunks are retained exactly once, in order, with
  independent copies; caller buffer mutation does not alter queued data.
  Partial reads preserve the remaining bytes.

Each directory records the instrumented assembly hashes, hook script hash,
constructor source hashes before/after the hook, patch provenance, and logs.
The production patch contains no scheduling hook.

## Uninstrumented final driver

The exact eleven-patch driver used for the fresh parallel protocol cohort
passes **7/7** [uninstrumented checks](qualified-uninstrumented/run.log).
[Assembly hashes](qualified-uninstrumented/result.json) and
[patch provenance](qualified-uninstrumented/driver-provenance.json) identify it.
These checks assert that the scheduling hook is absent, use the public
constructor and real socket callback, and verify copied/FIFO data in both
transport modes, ignored null/empty input, callback removal on Dispose,
rejection of sends before authentication, and rejection of null/lossy sockets.
They are separate from the deterministic instrumented race proof.

The parent report records full standard/bridge validation of the repaired
uninstrumented driver and retains the failed intermediate cohort. No failed
test is silently retried by the harness.

## Reproduction

Run public callback checks against any prepared uninstrumented driver:

```sh
python3 run.py --suite-bin /absolute/path/to/suite/Bin --output /tmp/udp-publication-checks
```

Build isolated scheduling variants from the clean pinned Microsoft checkout:

```sh
python3 build-instrumented.py --source /absolute/path/to/WindowsProtocolTestSuites --output /tmp/udp-publication-baseline --variant baseline
python3 build-instrumented.py --source /absolute/path/to/WindowsProtocolTestSuites --output /tmp/udp-publication-fixed --variant fixed
python3 run.py --suite-bin /tmp/udp-publication-baseline/Bin --output /tmp/udp-publication-baseline-checks --instrumented
python3 run.py --suite-bin /tmp/udp-publication-fixed/Bin --output /tmp/udp-publication-fixed-checks --instrumented
```

The fixed build script was exercised from a fresh export and its output passed
[9/9 instrumented checks](reproduction-script-checks/run.log).

The baseline command deliberately omits only the callback-publication repair;
all other registered patches remain applied. The baseline regression returns
nonzero because the two forced early callbacks reproduce the bug. Build and
source instrumentation take place in temporary exports, never in the original
Microsoft checkout or prepared production driver.

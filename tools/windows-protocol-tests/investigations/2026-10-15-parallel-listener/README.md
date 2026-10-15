# Parallel listener and cleanup investigation — 2026-10-15

The original listener-startup failure exposed a real readiness defect in the
Microsoft driver and a port-allocation race in the Python harness. Full-suite
parallel validation also exposed cleanup, disconnect-ordering and UDP/TLS races.
No native FreeRDP source or binary changed during this investigation.

## Original failure and confirmed mechanism

The historical `restored-full-bridge-20261015/bridge-30` run reported a listener
at 12:41:06.275, queued a transport exception 12 ms later, then received a client
connection refusal. The old adapter discarded the nested socket exception.
A port collision is plausible, but the original error cannot be recovered.
The [prior restoration report](../2026-10-15-restored-negative-tests/README.md)
retains that failed attempt and its subsequent serial qualification.

The pinned Microsoft TLS transport returned from `Start` before its accept
thread bound and listened. Meanwhile, the Python harness reserved an ephemeral
port and closed the reservation before Microsoft bound it. This allowed a
premature connection or another socket to claim the port.

A [controlled occupied-port reproduction](driver/baseline/result.json) against
the actual Microsoft SDK showed `Start` returning successfully, followed by a
wrapped `AddressAlreadyInUse` error. This establishes the defect and hidden
error mechanism, not the exact historical socket error.

The repaired driver binds and listens synchronously, retains an OS-selected
port, and publishes that endpoint atomically. The runner verifies the case
token, IPv4 loopback address, concrete port and owned driver process group before
launching the client. It does not connect to probe readiness. Separate runner
processes have private suite copies, control sockets and atomically allocated
output directories. A 100 ms socket poll replaces idle accept-loop busy spinning.
Startup errors now retain their complete nested exception.

See the [driver investigation](driver/README.md),
[listener patch](../../patches/listener-startup-readiness.patch), and
[harness regressions](harness/README.md). The token and process-group checks
prevent stale/cross-case handoffs; they are not isolation from another process
running as the same OS user. This optional handoff is limited to the current
negotiated-TLS/IPv4 profile.

## Additional failures exposed by four workers

The [initial four-worker record](initial-parallel.json) preserves all **480**
results, per-case startup metadata, exact driver provenance, and a simultaneous
four-listener socket snapshot. It had **476 passes and four failures**. Listener
startup succeeded in every case; the four failed executions remain failures.

| Worker/case | Observed failure | Investigation and repair |
| --- | --- | --- |
| Standard A / 55, MCSConnectResponseFailure | Protocol assertion passed; cleanup returned EPERM. | Recheck/reap the owned child after a denied process-group signal; suppress only when its exit is confirmed. |
| Bridge B / 53, InvalidEncodedLength | Protocol assertion passed; cleanup returned EPERM. | Same cleanup repair. |
| Bridge B / 85, LicenseError_InvalidTKPKLength | Protocol assertion passed; cleanup returned EPERM. | Same cleanup repair. |
| Bridge B / 54, InvalidH221NonStandardkey | Expected-disconnect check rejected an unexpected transport event after successful startup and client rejection of the malformed response. | Preserve a racing retry connection and its packets for the next transport consumer while still requiring the original connection's disconnect. |

On macOS, an exited but unreaped child can cause `killpg` to return EPERM.
The [independent reproduction](harness/cleanup-race.json) observed this on
**32/32** children; a deterministic regression fails against the committed old
cleanup implementation. A permission failure for a child still running remains
an error, including during forced cleanup. The original three logs did not
retain a syscall traceback, so their precise syscall locus is inferred from
cleanup timing and reproduction. New adapter diagnostics retain the command,
stage, exception type and traceback.

An intermediate four-worker rerun found another cleanup EPERM in standard B,
case 55. Its new traceback identifies `Command2` → `client.stop` →
`killpg(SIGTERM)`. The initial immediate-poll repair was incomplete: macOS can
reject the signal while the child is exiting but before it becomes reapable.
A [100-exit experiment](harness/cleanup-transition.json) with the real native
client observed three such windows; all three children were reaped within milliseconds by a bounded wait.
The final cleanup helper waits up to the existing five-second phase limit and
suppresses EPERM only after confirmed exit. On timeout it preserves the original
permission error. The [intermediate record](intermediate-parallel.json) retains
that unsuccessful qualification attempt separately.

The intermediate run finished at **478/480**: the cleanup failure above and
bridge A / case 114 (`S3_DataTransfer_V2_ClientReceiveData`). The latter reached
RDPEUDP2 and then threw `NullReferenceException` at the Microsoft TLS channel's
`innerStream.AddReceivedData` call. Its constructor published the receive
callback before creating `innerStream`, allowing the UDP receive thread to
invoke a partially initialized object. This stack and source order identify the
defect directly. The later authentication/socket timeouts are consequences of
the receive loop stopping. The repair initializes callback state before
subscribing; it does not increase timeouts or alter native transport behavior.

The H221 failure log omitted the unexpected event type. Its client trace shows
an initial transport retry racing cleanup; a controlled SDK transport-queue
regression establishes that `Connected` could overtake the old connection's
`Disconnected` event. That exact historical event identity remains unrecorded.
The [ordering investigation](driver/disconnect-ordering/README.md) retains the
trace, failing baseline controls and repaired results. The new queue preserves
connection/packet order. A new connection by itself, another session closing,
or a decoder exception cannot satisfy the original session's disconnect.

## Validation and boundaries

Final integration qualification is recorded in
[validation-parallel.json](../../validation-parallel.json). The final driver has
eleven pinned patches. All four full extended workers passed **120/120** cases,
for **480/480** executions (240 standard and 240 bridge). Each worker includes
the six quick cases and all five restored negative tests. A read-only socket
snapshot proves four distinct live loopback listeners overlapped in time; the
qualification guard checks that snapshot against per-case timestamps and IDs,
and checks every worker's options, action counts, results and error fields.

| Final worker | Passed | Failed |
| --- | ---: | ---: |
| Standard A | 120 | 0 |
| Standard B | 120 | 0 |
| Bridge A | 120 | 0 |
| Bridge B | 120 | 0 |

The actual-SDK checks on that combined build passed:

- **9/9 listener checks:** synchronous startup, occupied-port errors/resource
  release, restart cycles, 32 simultaneous listeners, TLS/CredSSP listener
  paths, and unchanged raw-RDP fixed-port startup. See
  [listener results](driver/qualified-eleven/result.json).
- **12/12 disconnect checks:** the original strict negative controls plus
  racing connections, preserved packets, FIFO ordering and errors. The old
  driver passed 7/12 and failed all five added ordering checks. See
  [ordering results](driver/disconnect-ordering/qualified-eleven/result.json).
- **7/7 UDP/TLS channel checks** on the uninstrumented final driver: ordered,
  copied receive bytes in both AutoHandle modes, null/empty input, disposal,
  unauthenticated-send rejection and invalid-socket controls. See
  [callback results](driver/udp-tls-publication/qualified-uninstrumented/result.json).
  The [callback investigation](driver/udp-tls-publication/README.md) separately
  labels the baseline/fixed scheduling-hook experiment; its hook is absent from
  the production patch and all live protocol runs. The instrumented baseline passed
  7/9 checks and reproduced the crash in both forced early callbacks; the fixed
  variant passed 9/9.

All **47 harness and qualification checks** passed, including per-worker
coverage/options/action counts and verified listener overlap. The
[complete log](final-harness-tests.log) is retained. Catalog regeneration and
its consistency check preserve the 432-test inventory, 120 implemented cases,
and the existing quick/extended profile membership.

Concurrent qualification means four independent runner processes, each running
its own cases serially. Two workers use standard mode and two use bridge mode;
there is no new `--jobs` option and `--mode both` remains serial. No failed case
is retried automatically, and no error or required assertion is downgraded.

This remains macOS arm64/TLS protocol qualification. CredSSP listener unit
checks do not validate Windows SSPI/NLA authentication. Graphics cases observe
protocol acknowledgements rather than pixels; drive cases do not validate file
I/O. The quick profile remains six cases and extended remains 120.

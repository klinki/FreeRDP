# Microsoft listener startup investigation

## What the historical failure proves

The retained `bridge-30` trace reported listener startup success at
12:41:06.275, then an asynchronous transport exception at 12:41:06.287;
the client subsequently received connection refused. The adapter logged only
`ex.Message`, discarding the nested socket exception. The original socket error
cannot be recovered from that trace. In particular, a competing user of the
harness's previously reserved-and-released ephemeral port is plausible, but is
not established as the historical cause.

The pinned Microsoft directed transport created, bound, and listened on its
socket inside `AcceptLoopInternal`, after `Start` had returned success. That
permits both a client arriving before listen and an asynchronous bind failure
being reported as an unrelated connection timeout. The general TCP transport
used for Standard RDP Security already starts its listener synchronously.

## Deterministic reproduction

[Program.cs](Program.cs) executes the actual prepared Microsoft SDK assemblies,
without copying their listener implementation. `--baseline` occupies a real
loopback port, calls `RdpbcgrServer.Start`, and then calls `ExpectConnect`.
The original eight-patch driver returned success from Start for the occupied
port, then raised a generic `InvalidOperationException` wrapping
`SocketException: AddressAlreadyInUse` (macOS native error 48). See
[baseline/run.log](baseline/run.log) and its recorded assembly hashes.

This proves the mechanism that hides startup failures, not the unavailable
original exception or a deterministic spontaneous collision in `bridge-30`.
An initial sandbox-only attempt was denied local socket binding; it was not
used as protocol evidence. The recorded reproduction and all listener checks
were executed with loopback socket permission.

## Repair and configuration boundaries

The [listener patch](../../../patches/listener-startup-readiness.patch) moves
socket creation, bind, listen, and nonblocking setup onto the Start caller's
thread. It releases partial resources and rethrows the original exception on
failure. The accept thread starts only after listen succeeds. A 100 ms socket
poll replaces the former idle WouldBlock busy loop; this reduces parallel CPU
load and is not claimed as the historical failure's cause. Stop closes the
socket, joins the accept thread, disposes its cancellation source, resets
started state, and suppresses only disposal/socket exceptions during requested
shutdown. Genuine asynchronous failures retain their full nested exception.

This SDK path serves negotiated TLS, negotiated CredSSP, and direct CredSSP.
Their bind/readiness behavior is checked on macOS; the direct CredSSP check
never authenticates a peer. It does not qualify Windows SSPI/NLA authentication.
Standard RDP Security uses the existing general TCP transport, which remains
unchanged and has a fixed-port connection regression. RDSTLS uses the suite's
negotiated TLS infrastructure, but RDSTLS authentication is not qualified here.

The optional runner handoff uses `FREERDP_WPTS_LISTENER_FILE` and
`FREERDP_WPTS_LISTENER_TOKEN`. It is explicitly limited to negotiated TLS over
IPv4 and binds only loopback. Configured port zero is selected and retained by
the real kernel listener. `RdpbcgrServer.ServerPort` and the adapter's runtime
port report that concrete port. The SUT-control payload reads it after startup
instead of using the zero cached during adapter initialization. Accepted session
`LocalIdentity` naturally carries the actual endpoint into UDP/multitransport.

After successful listen, the driver atomically publishes JSON with version,
token, address, port, and driver process ID. It never opens a readiness TCP
connection. A repeated Start retains the same port and accepts an existing
publication only when its complete content is identical; changed publication
fails. Existing fixed-port users without these environment variables retain
their address/security selection. General RDP port-zero discovery is not added.

## Regression results

All nine [actual-SDK checks](qualified-eleven/run.log) passed again against the
final combined eleven-patch driver
(`/private/tmp/wpts-parallel-suite-qualified-20261015/Bin`); its
[exact patch provenance](qualified-eleven/driver-provenance.json) and
[assembly hashes](qualified-eleven/result.json) are retained. The initial isolated
patched run remains under `patched/`, and the earlier nine-patch combined run
under `final/`. The separate [disconnect-ordering investigation](disconnect-ordering/README.md)
explains the tenth patch discovered during full parallel qualification. The
[UDP/TLS publication investigation](udp-tls-publication/README.md) explains the
eleventh patch. The ten-patch check remains under `qualified-final/`. Checks:

- TLS startup owns a nonzero OS-selected port and accepts a connection immediately.
- Negotiated CredSSP listener startup does the same before authentication.
- Direct CredSSP listener startup owns its port without attempting SSPI authentication.
- Each of those three security modes throws AddressAlreadyInUse from Start,
  releases socket/thread/cancellation state, permits a retry after the conflict
  is removed, and releases the port for an independent listener.
- Twelve fixed-port stop/restart cycles leave no queued shutdown exception.
- Thirty-two concurrent port-zero listeners own distinct ports and all accept
  real connections; accepted session endpoints match their published SDK ports.
- Standard RDP Security still accepts a fixed-port TCP connection.

Reproduce against a chosen prepared driver:

```sh
python3 run.py --suite-bin /absolute/path/to/suite/Bin --output /tmp/listener-checks
python3 run.py --suite-bin /absolute/path/to/original-suite/Bin --output /tmp/listener-baseline --baseline
```

The runner copies the selected exact assemblies into an isolated temporary
build and records their hashes, SDK version, program hash, pass/fail names, and
raw output. Both the initial isolated fixed build and final combined driver are recorded
here; the parent investigation records standard/bridge protocol qualification.
No native FreeRDP change was needed for this listener repair.

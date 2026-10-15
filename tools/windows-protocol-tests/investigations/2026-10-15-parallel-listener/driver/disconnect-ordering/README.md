# A new connection racing an expected disconnect

The first four-worker extended run exposed an additional Microsoft driver
ordering failure in `S1_ConnectionTest_BasicSettingExchange_NegativeTest_InvalidH221NonStandardkey`
(`bridge-b`, case 54). Listener startup succeeded, then the TCP/TLS/MCS sequence
completed up to the malformed server response. This was not another startup
failure.

The [client log](live-observed/client-1.log) explicitly rejects the invalid H.221
key at 14:44:54.914 and then begins initializing another transport attempt.
The [driver log](live-observed/driver.log) subsequently throws `Unknown object
received in transport` from the strict disconnect wait. That message omitted
the event type and endpoint. The directed transport emits only Connected,
Disconnected, ReceivedPacket and Exception events, and that branch is reached
only for Connected among those types. A new-connection event racing the old
connection's receive-thread EOF is therefore the source-supported explanation;
the original log itself did not record the event identity.

The actual-SDK regression uses controlled events in the real Microsoft transport
queue to prove this ordering bug deterministically. It does not depend on
reproducing the client's timing or treat its retry as disconnection evidence.
The nine-patch driver passes the existing seven strict-disconnect controls but
fails all five newly added ordering checks: [baseline/run.log](baseline/run.log).

The [separate ordering patch](../../../../patches/listener-disconnect-event-ordering.patch)
defers only Connected and packets for sessions not yet known to the SDK. Normal
transport consumers replay those events in FIFO order. Packets for known
sessions retain the existing per-session buffer. The disconnect wait reads the
underlying transport directly so it cannot repeatedly consume the events it
just deferred. Only an actual Disconnected event for the requested session
satisfies the wait; decoder errors and another session's disconnection still
fail. No disconnection or exception is put in the deferred queue. Unexpected
future event types now include their type and endpoints in the failure message.

All twelve [final combined-driver checks](qualified-eleven/run.log) pass against
the exact eleven-patch driver used for the fresh four-worker protocol runs.
[Assembly hashes](qualified-eleven/result.json) and
[patch provenance](qualified-eleven/driver-provenance.json) are retained, along
with the earlier isolated [patched result](patched/run.log). They cover:

- A new connection followed by the original session's disconnect succeeds,
  and the new connection remains available to a subsequent ExpectConnect.
- Known-session initial packets remain available in order through ExpectPdu.
- Unknown-session connection and packet events retain transport FIFO order.
- A new connection without the original disconnect reaches the bounded timeout
  and does not remove the original live session.
- Multiple racing connections survive a decoder error in order; the error is
  still propagated.
- Existing controls still enforce ordinary-packet timeout, exact-session
  disconnection, packet preservation, decoder failures, wrong-session rejection,
  TLS EOF, and TLS reset handling.

Run against a prepared driver with:

```sh
python3 run.py --suite-bin /absolute/path/to/suite/Bin --output /tmp/disconnect-ordering-checks
```

The reproduction controls transport events, never copies the SDK's disconnect
implementation, and records exact loaded-assembly hashes. The parent report
retains the failed four-worker attempt and records fresh end-to-end validation
with the repaired combined driver. No retry was added to hide a failed test,
and no native FreeRDP code was changed for this repair.

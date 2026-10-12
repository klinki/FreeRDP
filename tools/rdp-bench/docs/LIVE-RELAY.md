# Live VM relay

`live_relay.py` forwards an RDP session from `127.0.0.1:43389` to
`192.168.64.2:3389` by default. Both endpoints are configurable. Each client UDP
source address gets its own connected backend socket and return path. TCP
connections preserve the byte stream, apply bounded backpressure and drain a
half-closed stream before closing the opposite write direction.

**This is UDP-only impairment. TCP control and fallback traffic are unshaped.**
It cannot establish a full-RDP 10 Mbit/s network profile. UDP byte counters count
all datagrams, including protocol headers, retransmissions and duplicates; TCP
byte counters measure bytes forwarded by the proxy. Neither is verified
application goodput. Negotiate UDP and verify activity in both directions before
interpreting a VM trial as exercising the UDP treatment.

The relay uses the existing `rdpbench.link.Link` scheduling model without changing
RDP bytes. At 10 Mbit/s a 1222-byte datagram occupies 1 ms after adding 28 bytes of
IPv4/UDP overhead. `ten_mbit_clean` adds 15 ms each way without jitter or loss;
`ten_mbit` adds 15 ms each way, uniform ±1 ms jitter and seeded 0.0005 random loss
in each direction. Link queue limits, duplication, reorder and blackout settings
are also honored. Rate decisions are based on relay arrival times; identical
seeds repeat impairment decisions by packet ordinal, not entire live sessions.
The server-to-client seed is the requested seed plus 1,000,003, matching the
transport suite.

## Adapter contract

Configure this command prefix in the live benchmark controller:

```sh
python3 tools/rdp-bench/live_relay.py adapter \
  --state /absolute/task/network-state.json \
  --output-dir /absolute/task/network-artifacts \
  --listen-host 127.0.0.1 --listen-port 43389 \
  --target-host 192.168.64.2 --target-port 3389
```

The controller appends one base64-encoded JSON object. Apply requires:

```json
{
  "op": "apply",
  "run_id": "owned-trial-id",
  "profile_id": "ten_mbit",
  "profile": {"c2s": {}, "s2c": {}},
  "seed": 29,
  "lease_s": 120,
  "trial_dir": "/absolute/trial/directory"
}
```

Supply the complete direction objects from `configs/profiles.json`, rather than
the empty objects shown above. `trial_dir` is optional. When present, the lease's
artifacts go under `trial_dir/live-network/lease-<id>`; otherwise they go under
`--output-dir`. Each lease creates fresh artifacts and retains previous ones.
Allowed lease lengths are 0.25 through 3600 seconds.

Status and restore requests contain `op` and the same `run_id`. Status may also
include `profile_id`, which must match. A different run cannot restore another
owner's relay. Apply refuses a live or unverifiable existing state. A recorded,
verified terminal lease permits another apply using the same state path and
listener port. There is no system-wide shaping or cleanup by guessed PID.

After measurement and drain, send an owned status request with
`"begin_teardown": true` **before** cancelling the client. This records a
monotonic lifecycle marker without closing the relay. TCP `socket_errors` retains
every error. `active_socket_errors` excludes only an established client-side
`ECONNRESET` with zero pending bytes after that explicit marker; such resets count
as `teardown_client_resets`. Target errors, resets before the marker, and any
discarded buffered data remain active failures. The ledger retains errno, peer
side, operation, classification and marker timestamp for each TCP close.

Apply returns success only after the child has bound UDP, TCP and its private
control listener and answered a live authenticated control round trip. The child
owns an independent monotonic expiry deadline. Restore returns success only after
the owned child has closed its sockets and written its final status. A stale state
file alone never establishes a live relay. Binding conflicts produce a failed
lease artifact and a nonzero adapter exit without claiming readiness.

## Evidence

Receipts include the exact resolved profile, requested seed, direction seed,
ownership identifiers, bound endpoints, remaining lease, TCP connection counts,
per-direction UDP counter hits, queue/random/blackout drops, socket failures,
pending datagrams and bytes, and dispatch lateness p50/p95/p99/max. The
`packet_accounting.balanced` check includes received originals, generated copies,
delivered packets, modeled drops, socket send failures and packets still pending.
Pending packets at expiry are retained as pending, never counted as delivered.

Live receipt p50/p95/p99 values are conservative upper bounds from a bounded
histogram of every delivery: 10 microsecond bins through 10 ms, 1 ms bins through
1 second, then 1 second bins. `max` is exact. The method is stated in each
direction's `lateness_quantile_method`; the upper bound may slightly exceed the
exact maximum in a small sample. Raw dispatch lateness remains exact in the
ledger. This avoids repeatedly sorting up to 200,000 timing samples inside the
forwarding loop when a controller requests status. A 1.5 ms upper-bound guard
therefore never accepts an exact p99 above 1.5 ms.

`capturepath` refers to `relay.jsonl`, **a userspace metadata ledger, not a packet
capture**. It records offer/drop/dispatch monotonic timestamps, packet ordinals,
copy identifiers, visible protected UDP2 header fields when classifiable, datagram
lengths and TCP connection metadata. Unknown handshake packets remain unknown.
No payload, credentials or TLS secrets are recorded. Use an actual OS packet
capture separately for wire-level decoding.

Expired deliveries run before and after each selected I/O handler and between
bounded UDP arrival batches. Diagnostic `scheduler_gap` rows identify selector
interval overshoot or long I/O-handler intervals above 1 ms. These intervals
include host scheduling; they establish where a delay was observed, not exclusive
causation. Startup queue drops from a real burst remain modeled drops and are
retained separately from dispatch lateness.

A bounded asynchronous writer keeps JSON formatting and disk writes outside the
scheduling loop. Its overflow, pending records, output errors and final close
state are visible. Packet dispatch still uses real OS scheduling, so observe
lateness and calibrate the host before making precise latency or throughput
claims. UDP kernel drop counters are unavailable and explicitly `null`.

Run socket regressions on a host that permits loopback binds:

```sh
python3 -m unittest discover -s tools/rdp-bench/tests -p test_live_relay.py -v
```

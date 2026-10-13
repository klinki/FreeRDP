# UDP2 recovery after a lost channel

The previous 256-slot channel-map overflow was repaired in `90b02e86b`.
The next failure was in a different structure: the 128-entry DataSeq receipt
bitmap automatically advanced its lower edge when a later DATA packet fell
outside that bitmap. This discarded unseen datagrams and moved the ACK base
past them. Channel payload storage correctly waited for the missing bytes,
while receipt feedback could tell Windows to retire their transmission.

## Captured reference

The diagnostic Windows 25% run in `build/udp-recovery-20261013/ref25` failed
after about 26.15 measured seconds. Channel `a0dc` remained missing for
16.118 seconds; the client exited with code 147 with 3,686 retained chunks.
Windows offered that channel as DataSeq `b6e3` in packet 40,073 of the
pre-impairment capture. No matching channel reached the post-impairment
capture. Both capture processes exited normally with zero kernel drops.

The receiver ledger recorded an automatic base advance `b6e3` → `b6e4`
at event 49,744, followed by a successful ACK vector with base `b6e4` at
event 49,745. Its bytes exactly match actual outgoing packet 40,294. The
last received AckOfAcks was still `b6e3`, which does not retire that label;
the first AckOfAcks past it arrived about 19.44 ms after the invalid vector.
Later cumulative ACKs also cover the label after this feedback/retirement
sequence; those later ACKs alone would not establish incorrect receipt.
The detailed packet/AckOfAcks ordering and qualification are in
[the compact causal audit](../results/recovery-causal-proof-20261013.json).
There was one offered attempt for this channel in the recorded interval.
The capture establishes observed behavior at the client boundary; it does
not expose Windows' internal retransmission decisions.

The unmodified reference ledger is retained. Its strict validity check fails
because the ARM event and its triggering receive event have timestamps
reversed by 209,208 ns. There are no missing records or trace-buffer drops;
the later target events have separate packet corroboration. The ARM ordering
bug is repaired for future traces. This reference is diagnostic evidence,
not a controlled FPS baseline: enabled tracing hashes/copies events under
the lock and exports JSON on failure.

## Recovery invariant

The [receiver-window rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/0490fcf0-75fa-4362-a6c8-496732bce39b)
allow lower-edge advancement through an actually received prefix or valid
sender AckOfAcks retirement. A later DATA packet alone does not authorize it.
The [ordinary ACK](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/32be8113-cdb4-4843-809c-3fa3aa886971)
remains cumulative. AckOfAcks retirement does not discard required channel
payloads or authorize skipping a missing channel.

Receipt tracking now uses tagged slots across the 32,768-position active
sequence range. Tags distinguish wrap epochs, and counts/span bookkeeping
keep normal receipt and gap checks constant-time. It retains holes rather
than sliding them out because of higher arrivals. Initial far-start
compatibility remains explicitly restricted to the first DATA before
AckOfAcks establishes the epoch.

Each ACK vector represents a truthful prefix within the 127-coded-byte
wire limit. Its timestamp describes the highest positive receipt actually
encoded; an all-missing encoded prefix has no receipt timestamp. The next
channel still waits for its real bytes. The 32 KiB sender queue and sixteen
second channel-gap watchdog are unchanged.

## Diagnostic tracing

Set `RDPEUDP_RECOVERY_TRACE` to a new output filename to enable the bounded
ledger. `RDPEUDP_RECOVERY_TRACE_ARM` optionally names a marker file: create
it after warmup, before the measured loss step. `RDPEUDP_RECOVERY_TRACE_LIMIT`
sets capacity up to 1,048,576 events. Default capacity is 262,144 events,
about 50 MiB; the maximum is about 200 MiB per association. Overflow makes
the trace invalid. Trace files are exclusively created and flushed at
failure/teardown. They contain transport metadata and short ciphertext
hashes, with no payload contents or TLS keys. Leave tracing disabled for
performance runs unless its overhead is measured separately.

`packet_audit.py` independently decodes the visible transport headers in
classic PCAP files and checks capture receipts and the receiver ledger:

```sh
python3 tools/rdp-bench/packet_audit.py \
  --offered build/udp-recovery-20261013/ref25/offered.pcap \
  --received build/udp-recovery-20261013/ref25/received.pcap \
  --vm-ip 192.168.64.2 --channel 0xa0dc \
  --capture-health build/udp-recovery-20261013/ref25/capture-health.json \
  --trace build/udp-recovery-20261013/ref25/recovery.jsonl \
  --output build/udp-recovery-20261013/ref25/channel-a0dc-audit
```

That reference command deliberately returns failure for the strict startup
timestamp defect. Capture absence alone never becomes proof of sender
abandonment. Sequence epochs, valid AckOfAcks retirement, capture location
and kernel-drop receipts must accompany a causal interpretation. The captures
use 256-byte snaplen and may contain encrypted body prefixes; the audit exports
decoded transport headers.

## Windows method and results

The rerun uses the pinned OpenThinRDP coloured-noise workload, Linux Xvfb
1280×800, progressive graphics with AVC disabled, four client-container CPUs
and 4 GiB memory. After at least 45 seconds of workload warmup, each requested
loss level runs for 120 seconds or until client termination. The original
25% → 50% → 70% ramp stops at the first exit; any unreached levels get one
separately labelled fresh session. Failed measured levels are never retried
to select a survivor.

Loss applies to all IPv4 arriving from Windows; packets of at least 512 IP
bytes additionally receive 1 ms delay and 70% reordering. There is no rate
cap. An owned veth replaces IFB; upstream ICMP verification is omitted on this
route. Pre/post capture placement was independently verified with a clean
TCP/TLS probe and a 100% loss probe. Netem counters retain actual completed
drop fractions. Netem and the upstream noise RNG are unseeded.

The corrected Windows collector runs one continuous PDH query. A baseline
is retained without rates; subsequent samples cover adjacent measured
intervals. The diagnostic run verified 10.014–10.077-second cadence. Invalid
providers and boundary-crossing/clock-uncertain samples are excluded, with
all errors retained. Server output FPS and server RTT counters do not
establish physical client FPS, verified application goodput or input-to-display
p95/p99 latency.

[Campaign JSON](../results/recovery-20261013.json),
[CSV](../results/recovery-20261013.csv) and
[readable results](../results/recovery-20261013.md) preserve every outcome.

## Regression coverage

The native receiver cases inspect actual emitted ACK fields while DataSeq 1
is missing and 300 arrives, recover the logical channel on fresh attempt 301,
then permit cumulative advancement only after peer AckOfAcks retirement.
They also cover the far edge, unacknowledged exact half-space ambiguity,
tagged slot aliases at 0/32768, and wraparound. Existing long wrap fixtures
now explicitly provide sender retirement rather than depending on automatic
local window sliding.

The ACK timing case uses 1,600 alternating receipt states to verify a truthful
889-state prefix encoded in 127 bytes and its represented-positive timestamp.
An 8,001-state all-negative prefix verifies omission of the timestamp rather
than borrowing the arrival time of an unrepresented later receipt. Independent
Python fixtures cover PCAP framing, transport layouts, ACK semantics,
malformed/truncated headers, capture health, sequence/clock completeness and
the Windows opaque flag observed in the actual trace.

The seven native transport/command-line targets passed in 3.56 seconds;
all 280 Python tests passed with the actual development native library.
The production Linux X11 client also built successfully with benchmark
instrumentation disabled by default.

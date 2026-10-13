# UDP2 recovery causal evidence — ref25

The receiver advanced its DataSeq lower edge across an unreceived attempt,
then emitted an ACK vector advertising the advanced boundary. This is
corroborated by packet bytes, not inferred solely from absent retransmissions.
The [compact JSON](recovery-causal-proof-20261013.json) contains input hashes,
packet identities and the exact referenced receiver events.

| Evidence | Reference |
|---|---|
| Channel `a0dc`, original DataSeq `b6e3`, 639 encrypted body bytes | Offered pcap packet **40073** |
| Target attempts in covered interfaces | **1 offered, 0 post-loss**; no replacement alias observed |
| Incoming `b763` is exactly 128 ahead of `b6e3` | Offered **40292**, received **17445**, trace **49743** |
| Local lower edge moves `b6e3 → b6e4`; prior bit zero is unreceived | Trace **49744** |
| Last peer AoA before that slide equals `b6e3`, so it does not retire `b6e3` | Trace **49725** |
| Successful ACKVEC base `b6e4`, exact encoded fields below | Offered **40294**, trace **49745**, byte-for-byte match |
| First later peer AoA past the target is `b75f` | Offered **40303**, received **17452**, trace **49760** |

The first emitted vector's raw ACKVEC payload is
`e4b693d3390b007a6dc47e2eca4e317bc8767d7ec46671011b08`.
It has base `b6e4` and its first represented state remains pending. The bad
base appears **53.773 ms after the original offer**. Windows' first retiring
AoA follows it by **19.439 ms**. The relevant captured DataSeq range is
`5e5a–ca55`, with no forward wrap and only one occurrence of the target
DataSeq, so a reused 16-bit label does not explain this match.

Microsoft's [receiver-window rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/0490fcf0-75fa-4362-a6c8-496732bce39b)
permit advancing the lower edge after acknowledging a received prefix or
receiving a peer AoA. A higher incoming DataSeq alone does not authorize
discarding this pending entry. The observed first bad feedback is an
**ACK vector with an invalid lower boundary**, rather than a positive bit
explicitly acknowledging `b6e3`. Later plain ACKs are cumulative under
[3.1.5.6](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/32be8113-cdb4-4843-809c-3fa3aa886971),
but they occur after peer AoA retirement and are not used as the initiating
false-receipt proof for this target.

Both pcap files are complete according to their stop receipts, with **zero
kernel drops and zero visible-header parse errors** (48,758 offered records,
21,072 received). Interface positioning depends on the recorded network
setup. Ciphertext bodies are snap-truncated; complete visible transport
headers remain decodable. Windows' opaque flag `0x200` is retained explicitly
and introduces no extra field in the correlated visible layouts.

The **whole receiver ledger is strictly invalid for chronology**: ARM event
0 is 209,208 ns later than the first RX event 1. The original file is
unchanged. This proof uses the separate, monotonic excerpt **49719–49777**,
more than nine seconds after ARM, and byte-exact pcap corroboration. The
producer recorded 60,253 events with no overflow, but that does not waive
the startup ordering defect or qualify whole-ledger absence claims.

Windows later advances AoA and no replacement for this channel appears in
the covered captures before the terminal channel gap. This is consistent
with invalid prefix feedback suppressing repair. Windows' internal retry
decision was not traced, so that final causal link remains an inference;
the receiver boundary violation and actual emitted feedback are directly
supported. The conclusion does not claim packet absence outside the
captured interfaces or interval, or application-byte integrity.

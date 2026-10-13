# Candidate packet recovery audit — 13 October 2026

The capture independently shows fresh Windows retransmission attempts reaching the client after an earlier attempt for the same channel was absent post-loss. One example is retained for each completed loss phase. Every selected example also has an actual outgoing client ACK positively covering the repaired attempt.

| Loss | Channel | Original DataSeq / offered packet | Fresh DataSeq / offered packet | Post-loss packet | Retry interval | Positive ACK packet |
| --- | --- | --- | --- | --- | --- | --- |
| 25% | `926e` | `977a` / 6,427 | `978d` / 6,462 | 2,785 | 10.573 ms | 6,466 (selective_positive) |
| 50% | `19a5` | `51f6` / 548,235 | `5218` / 548,294 | 234,243 | 12.339 ms | 548,331 (selective_positive) |
| 70% | `c5b3` | `afd9` / 1,076,126 | `b00c` / 1,076,191 | 409,478 | 12.280 ms | 1,076,199 (selective_positive) |

All 1,597,828 offered and 529,456 received PCAP records decoded with **zero visible-header errors**. Both capture receipts report complete capture and zero kernel drops. The offered capture includes 1,066,003 real Windows DATA packets and 528,909 client ACK packets; the post-loss capture includes 529,418 real Windows DATA packets.

The stream maintained at most 32,768 channel records. Offered DataSeq advanced by at most one per forward step, and channel sequence by at most two; neither had an exact half-space ambiguity. Unwrapped epoch labels prevent confusing a repeated 16-bit channel number after wrap with a retry. Selected original/alias pairs are less than one second apart and at least one second inside their measured phase.

The original and alias have equal original DATA-body lengths and identical first 32 captured ciphertext bytes (SHA-256 retained in the JSON). The alias has exactly one post-loss match across the full covered capture, using connection, visible-header hash, body length and ciphertext-prefix hash. The original has none. Packet indices are one-based within each named PCAP; hashes and exact timestamps permit independent rechecking.

**Limits:** 256-byte snap capture truncates encrypted bodies, so this verifies transport-boundary retransmission and arrival plus client positive feedback. It does not verify complete plaintext payloads, ordered application delivery, application goodput, or input/display tail latency. The retry interval is offer-to-offer time for these selected examples, not a latency distribution or an RTO estimate. The bounded 128 candidates per phase are examples rather than a complete count of repaired losses. Windows internal retry decisions are not traced.

[Machine-readable audit](recovery-candidate-packet-summary-20261013.json) retains capture SHA-256 hashes, packet/header/body-prefix hashes, connection identifiers, epoch labels, exact ACK wire bytes and qualifications.

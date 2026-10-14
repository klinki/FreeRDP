# Header-only recovery audit — compact summary

Channel `0a1f`; native gap **12058.863422 ms**; peak buffered chunks **964**.

Native flow association: **True**, `172.17.0.2:53851`.

Whole gap inside both observed capture windows: **False**.
Whole gap inside a measured loss step: **False**.
Native recovery after the zero-loss receipt: **True**.

Eligible captured active-attempt positive ACK references: **0**. Eligible violation references: **0**.
Historical positive references retired by peer AoA: **13000**.

Serial-ambiguous aliases: **0**; receiver resyncs in the focused window: **0**.
a receiver resync conservatively invalidates continuous-epoch active-ACK qualification for the focused window; does not imply capture corruption

| Offered DataSeq | Postloss matches | Native RX status counts | Eligible active positive ACK refs | AoA-retired positive refs |
|---|---:|---|---:|---:|
| `0cb8` | 0 | {} | 0 | 520 |
| `0d39` | 0 | {} | 0 | 520 |
| `0ddb` | 0 | {} | 0 | 520 |
| `0e81` | 0 | {} | 0 | 520 |
| `1003` | 0 | {} | 0 | 520 |
| `1196` | 0 | {} | 0 | 520 |
| `133b` | 0 | {} | 0 | 520 |
| `1453` | 0 | {} | 0 | 520 |
| `151d` | 0 | {} | 0 | 520 |
| `15ab` | 0 | {} | 0 | 520 |
| `1616` | 0 | {} | 0 | 520 |
| `16c7` | 0 | {} | 0 | 520 |
| `173a` | 0 | {} | 0 | 520 |
| `17c1` | 0 | {} | 0 | 520 |
| `183c` | 0 | {} | 0 | 520 |
| `18ad` | 0 | {} | 0 | 520 |
| `1926` | 0 | {} | 0 | 520 |
| `1978` | 0 | {} | 0 | 520 |
| `19b6` | 0 | {} | 0 | 520 |
| `19e7` | 0 | {} | 0 | 520 |
| `1a10` | 0 | {} | 0 | 520 |
| `1a2e` | 0 | {} | 0 | 520 |
| `1a42` | 0 | {} | 0 | 520 |
| `1a51` | 0 | {} | 0 | 520 |
| `1a5b` | 0 | {} | 0 | 520 |

Observed inter-alias offer intervals (ms): 41.568, 27.873, 332.103, 162.206, 40.727, 15.756, 13.736, 22.148, 15.469, 16.016, 33.041, 23.749, 17.876, 18.62, 28.109, 27.365, 30.7, 15.813, 16.185, 54.673, 40.446, 36.613, 91.298, 4036.262.

## Source hashes

Analysis: `817c33eba42cde0c044586e9975ba6a6da44db13803966c361b7d5f999999a6c`.
Compact generator: `65674cd5c337aa2c8746c18f6d37cb89e12cafb5caa79c928283c29a2a9007c4`.

- offered.pcap: `81a8efa19faa30ad1170bc158fb820004519de3798d46f195ba5851a0b882a51`
- received.pcap: `a751145c5d6229c090b088418d6376b49d582ed9088943b2ee063c6f155a9bfb`
- recovery-trace.jsonl: `ff3cfc83c6b73945acecd0e53545eaab8992ed7197335ee776ab792c37085ff7`
- capture-health.json: `d8aa334be1d89183c366b1da3f07dc8bf7397442d175ba3877fcb66258eac64f`
- campaign.json: `becb1c3726d4277cf8980eba46fbabcd788e558def13247b04f906eb960ee3fc`
- network-reset-zero.json: `27b5c55802eeac449886d16b60abfb1255407251b554f7c9bbee6820052b59a6`
- provenance.json: `7bfc2fc1b292fc2f766945e1df73dc2c6a8c7402e80b2b64bc3aadda5faa0988`

## Qualification

- This compact summary omits all per-packet/per-ACK arrays; detailed analyses and raw evidence remain local/in the ZIP.
- ACK counts are reference counts per alias, not unique ACK datagrams or global receipt totals.
- Historical AoA-retired attempts are separate from captured active-attempt receipt eligibility.
- Wire observations prove emitted/captured transport headers, not Windows acceptance, TLS plaintext or rendering.
- Capture bounds are first/last observed packet times, not recorder lifecycle timestamps.
- Native recovery beyond the capture or measured profile window cannot be described as captured recovery under that profile.
- The clock-slop envelope is declared; independent drift/clock-step review remains necessary.
- These observer-enabled diagnostics do not replace production benchmark failures.

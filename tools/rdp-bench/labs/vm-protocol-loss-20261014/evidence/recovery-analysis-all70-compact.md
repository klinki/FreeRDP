# Header-only recovery audit — compact summary

Channel `9b31`; native gap **819.790042 ms**; peak buffered chunks **780**.

Native flow association: **True**, `172.17.0.2:55736`.

Whole gap inside both observed capture windows: **True**.
Whole gap inside a measured loss step: **True**.
Native recovery after the zero-loss receipt: **False**.

Eligible captured active-attempt positive ACK references: **9**. Eligible violation references: **0**.
Historical positive references retired by peer AoA: **0**.

Serial-ambiguous aliases: **0**; receiver resyncs in the focused window: **0**.
a receiver resync conservatively invalidates continuous-epoch active-ACK qualification for the focused window; does not imply capture corruption

| Offered DataSeq | Postloss matches | Native RX status counts | Eligible active positive ACK refs | AoA-retired positive refs |
|---|---:|---|---:|---:|
| `6546` | 0 | {} | 0 | 0 |
| `65cc` | 0 | {} | 0 | 0 |
| `6667` | 0 | {} | 0 | 0 |
| `66d2` | 0 | {} | 0 | 0 |
| `67b1` | 0 | {} | 0 | 0 |
| `6851` | 0 | {} | 0 | 0 |
| `68ee` | 0 | {} | 0 | 0 |
| `6952` | 0 | {} | 0 | 0 |
| `69cb` | 0 | {} | 0 | 0 |
| `6a71` | 0 | {} | 0 | 0 |
| `6ae1` | 0 | {} | 0 | 0 |
| `6b28` | 0 | {} | 0 | 0 |
| `6bac` | 0 | {} | 0 | 0 |
| `6c85` | 0 | {} | 0 | 0 |
| `6d1d` | 0 | {} | 0 | 0 |
| `6d8e` | 0 | {} | 0 | 0 |
| `6e02` | 0 | {} | 0 | 0 |
| `6ed9` | 0 | {} | 0 | 0 |
| `6fa3` | 0 | {} | 0 | 0 |
| `7108` | 1 | {"1": 1} | 9 | 0 |

Observed inter-alias offer intervals (ms): 15.844, 37.939, 15.141, 63.541, 24.992, 11.24, 22.815, 8.892, 41.444, 15.934, 21.439, 45.415, 20.218, 12.704, 20.778, 99.697, 388.591, 129.089, 169.168.

## Source hashes

Analysis: `db34d6f9e175023e910f606e20220ce5360f22d07756ccdb5561e62a2edf7a42`.
Compact generator: `65674cd5c337aa2c8746c18f6d37cb89e12cafb5caa79c928283c29a2a9007c4`.

- offered.pcap: `afa59186b480c74580785c815bd3e6cded9785f402461a37854cde3de6a033d7`
- received.pcap: `e6c67f979676f194a78f7d3249090b996264fda3df98ead04358f1a457c8d217`
- recovery-trace.jsonl: `73651381878b64db78e28280b31943f98b2b69b9b1d4623b763654f25d2c5ed1`
- capture-health.json: `02b4b2049e7e052826e5f0e4255d9536117ad9448fb77b51208096ff5a469985`
- campaign.json: `da61492b6c2e9de0a4932097498eb9d5e4681b8cba7ce07c77df97df918a8363`
- network-reset-zero.json: `e01078d8a18430637c68d9575b4adff82bff39a89cdfbaf3515124059b9b497b`
- provenance.json: `6f3d5dcd00bac582ad7a37c615f07f686af6901fdd925d8538780ff5586e97f9`

## Qualification

- This compact summary omits all per-packet/per-ACK arrays; detailed analyses and raw evidence remain local/in the ZIP.
- ACK counts are reference counts per alias, not unique ACK datagrams or global receipt totals.
- Historical AoA-retired attempts are separate from captured active-attempt receipt eligibility.
- Wire observations prove emitted/captured transport headers, not Windows acceptance, TLS plaintext or rendering.
- Capture bounds are first/last observed packet times, not recorder lifecycle timestamps.
- Native recovery beyond the capture or measured profile window cannot be described as captured recovery under that profile.
- The clock-slop envelope is declared; independent drift/clock-step review remains necessary.
- These observer-enabled diagnostics do not replace production benchmark failures.

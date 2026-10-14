# Header-only recovery audit — compact summary

Channel `0465`; native gap **10848.235631 ms**; peak buffered chunks **1158**.

Native flow association: **True**, `172.17.0.2:46348`.

Whole gap inside both observed capture windows: **True**.
Whole gap inside a measured loss step: **True**.
Native recovery after the zero-loss receipt: **False**.

Eligible captured active-attempt positive ACK references: **0**. Eligible violation references: **0**.
Historical positive references retired by peer AoA: **34**.

Serial-ambiguous aliases: **19**; receiver resyncs in the focused window: **1**.
a receiver resync conservatively invalidates continuous-epoch active-ACK qualification for the focused window; does not imply capture corruption

| Offered DataSeq | Postloss matches | Native RX status counts | Eligible active positive ACK refs | AoA-retired positive refs |
|---|---:|---|---:|---:|
| `0d7b` | 0 | {} | 0 | 2 |
| `0ea6` | 0 | {} | 0 | 2 |
| `1011` | 0 | {} | 0 | 2 |
| `123c` | 0 | {} | 0 | 2 |
| `14c2` | 0 | {} | 0 | 2 |
| `16f8` | 0 | {} | 0 | 2 |
| `188a` | 0 | {} | 0 | 2 |
| `1a23` | 0 | {} | 0 | 2 |
| `1b38` | 0 | {} | 0 | 2 |
| `1bff` | 0 | {} | 0 | 2 |
| `1c94` | 0 | {} | 0 | 2 |
| `1d03` | 0 | {} | 0 | 2 |
| `1d58` | 0 | {} | 0 | 2 |
| `1d92` | 0 | {} | 0 | 2 |
| `1db9` | 0 | {} | 0 | 2 |
| `1dd1` | 0 | {} | 0 | 2 |
| `1ddf` | 0 | {} | 0 | 2 |
| `1de9` | 0 | {} | 0 | 0 |
| `1df4` | 1 | {"1": 1} | 0 | 0 |

Observed inter-alias offer intervals (ms): 310.729, 303.619, 421.244, 45.517, 117.441, 178.932, 39.415, 30.116, 19.419, 17.429, 15.95, 34.764, 17.831, 17.944, 68.763, 35.229, 207.859, 10535.335.

## Source hashes

Analysis: `60003d7bb350680786a8f250cc148e648576a8369fa1407904692504a79da4ad`.
Compact generator: `65674cd5c337aa2c8746c18f6d37cb89e12cafb5caa79c928283c29a2a9007c4`.

- offered.pcap: `28166e8ad5f851fd0930487ec116a2db4109e7d73736e3775b50fb889dd8b0ec`
- received.pcap: `650f3564fb30d04cb3d7ea606f7a794ae1ebcf172dd2f9c75fbd5ffdd6075634`
- recovery-trace.jsonl: `c5e0f98f0f9c1f4e8855e4de0aa388296345447a89ad5427df9c552b64ecff7b`
- capture-health.json: `117e1494adba3e5f069e9d8b1508db534200e047cbd831fd8faed65b4372fef8`
- campaign.json: `e316d362db0f34b1a536208d989fa767143e4fbd9b4ec065330d9576c74ba497`
- network-reset-zero.json: `015aa3b059b7a6c75ee25a293324c1f6bb5cd5d220781c062e59165462c979bd`
- provenance.json: `4e1e26a42a8b769fa052247d59edc21ebfb3a1fa32559fdae4997ebb1c9eafbf`

## Qualification

- This compact summary omits all per-packet/per-ACK arrays; detailed analyses and raw evidence remain local/in the ZIP.
- ACK counts are reference counts per alias, not unique ACK datagrams or global receipt totals.
- Historical AoA-retired attempts are separate from captured active-attempt receipt eligibility.
- Wire observations prove emitted/captured transport headers, not Windows acceptance, TLS plaintext or rendering.
- Capture bounds are first/last observed packet times, not recorder lifecycle timestamps.
- Native recovery beyond the capture or measured profile window cannot be described as captured recovery under that profile.
- The clock-slop envelope is declared; independent drift/clock-step review remains necessary.
- These observer-enabled diagnostics do not replace production benchmark failures.

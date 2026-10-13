# Native UDP2 loss and inflight-count matrix

Provisional; no accepted calibration receipt. Latency is observed transport probe RTT, not desktop latency.

| Loss | DATA limit | Runs / planned | Qualified | Full integrity | Failures | Goodput Mbit/s | RTT p95 / p99 ms | Missed / timeouts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0% | 16 | 5/5 | 0 | 4 | 2 | unavailable | unavailable / unavailable | 27 / 0 |
| 0% | 32 | 5/5 | 1 | 5 | 2 | 8.809 | 133.959 / 139.725 | 158 / 0 |
| 0% | 48 | 5/5 | 1 | 5 | 1 | 9.612 | 123.462 / 126.307 | 238 / 0 |
| 0% | 64 | 5/5 | 0 | 4 | 3 | unavailable | unavailable / unavailable | 40 / 0 |
| 25% | 16 | 5/5 | 0 | 2 | 5 | unavailable | unavailable / unavailable | 2792 / 0 |
| 25% | 32 | 5/5 | 0 | 2 | 5 | unavailable | unavailable / unavailable | 2155 / 0 |
| 25% | 48 | 5/5 | 0 | 3 | 5 | unavailable | unavailable / unavailable | 1443 / 176 |
| 25% | 64 | 5/5 | 0 | 2 | 5 | unavailable | unavailable / unavailable | 1782 / 294 |
| 50% | 16 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3401 / 21 |
| 50% | 32 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3288 / 0 |
| 50% | 48 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3181 / 0 |
| 50% | 64 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3077 / 0 |
| 70% | 16 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3484 / 0 |
| 70% | 32 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3460 / 0 |
| 70% | 48 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3430 / 20 |
| 70% | 64 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3428 / 0 |

16/32/48/64 are active DATA packet limits. Byte queues stay 128KiB, records 16KiB, and wire MTU 1232. All failures remain in JSON/CSV. Partial-prefix payload rates and survivor tails are diagnostic; they do not establish complete verified goodput.

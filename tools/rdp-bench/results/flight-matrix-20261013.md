# Native UDP2 loss and inflight-count matrix

Provisional; no accepted calibration receipt. Latency is observed transport probe RTT, not desktop latency.

| Loss | DATA limit | Runs / planned | Qualified | Full integrity | Failures | Goodput Mbit/s | RTT p95 / p99 ms | Missed / timeouts |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0% | 16 | 5/5 | 0 | 5 | 1 | unavailable | unavailable / unavailable | 1 / 0 |
| 0% | 32 | 5/5 | 0 | 5 | 0 | unavailable | unavailable / unavailable | 0 / 0 |
| 0% | 48 | 5/5 | 0 | 5 | 0 | unavailable | unavailable / unavailable | 0 / 0 |
| 0% | 64 | 5/5 | 0 | 5 | 3 | unavailable | unavailable / unavailable | 8 / 0 |
| 25% | 16 | 5/5 | 0 | 5 | 5 | unavailable | unavailable / unavailable | 223 / 8 |
| 25% | 32 | 5/5 | 1 | 4 | 1 | 5.162 | 920.226 / 1421.375 | 0 / 0 |
| 25% | 48 | 5/5 | 0 | 5 | 0 | unavailable | unavailable / unavailable | 0 / 0 |
| 25% | 64 | 5/5 | 0 | 5 | 0 | unavailable | unavailable / unavailable | 0 / 0 |
| 50% | 16 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 2855 / 0 |
| 50% | 32 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 2527 / 0 |
| 50% | 48 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3158 / 0 |
| 50% | 64 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 2982 / 0 |
| 70% | 16 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3466 / 0 |
| 70% | 32 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3450 / 0 |
| 70% | 48 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3436 / 0 |
| 70% | 64 | 5/5 | 0 | 0 | 5 | unavailable | unavailable / unavailable | 3422 / 0 |

16/32/48/64 are active DATA packet limits. Byte queues stay 128KiB, records 16KiB, and wire MTU 1232. All failures remain in JSON/CSV. Partial-prefix payload rates and survivor tails are diagnostic; they do not establish complete verified goodput.

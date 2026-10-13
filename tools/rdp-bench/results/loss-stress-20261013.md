# High-loss native transport campaign

raw native UDP2; independent random loss on both directions

| Configured loss each direction | Complete / failed | Qualified goodput Mbit/s (N) | Observed p95 / p99 ms (N) | Expected / injected / observed | Unserved / unobserved | First failure ms (N) |
|---|---:|---:|---:|---:|---:|---:|
| 0.050% | 5 / 0 | 4.059 (5) | 67.098 / 96.612 (5) | 3500 / 3500 / 3500 | 0 / 0 | unavailable (0) |
| 25.000% | 0 / 5 | unavailable (0) | 679.781 / 806.116 (4) | 3500 / 1063 / 998 | 2437 / 65 | 8351.377 (5) |
| 50.000% | 0 / 5 | unavailable (0) | unavailable / unavailable (0) | 3500 / 49 / 15 | 3451 / 34 | 1267.414 (5) |
| 70.000% | 0 / 5 | unavailable (0) | unavailable / unavailable (0) | 3500 / 17 / 2 | 3483 / 15 | 1553.699 (5) |

## Limits

- No accepted calibration receipt or live Windows performance claim is implied.
- Goodput summaries require artifact-intact timing-valid COMPLETE integrity-verified transfers; failed transfers are unavailable.
- p95/p99 are medians of per-trial observed-response percentiles; failed trials with responses remain diagnostic. Samples are never pooled.
- Latency is survivor-only: compare it with unobserved responses and unserved scheduled injections. Missing observations do not become zero.
- Probe counts include warmup; percentile samples include measurement-scheduled probes only.
- Early endpoint exit leaves responses censored/unobserved, not fabricated drain-deadline timeouts.
- Realized random loss uses eligible offers after queue/blackout drops; ACKs and retries also undergo random loss.
- Seeds reproduce packet-index impairment decisions; traffic changes do not preserve identical logical losses.

## Retained failures

- upload-loss25-candidate-default-s11-1faa45bd: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss25-candidate-default-s29-b58153c6: INVALID_HARNESS / FAIL: s2c relay p99 lateness 2.438ms exceeds 1.500ms; endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss25-candidate-default-s47-a63abbc5: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss25-candidate-default-s71-c67352a3: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss25-candidate-default-s101-2ccd9862: INVALID_HARNESS / FAIL: s2c relay p99 lateness 1.695ms exceeds 1.500ms; endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss50-candidate-default-s11-d5db0033: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss50-candidate-default-s29-30766cde: INVALID_HARNESS / FAIL: c2s relay p99 lateness 3.103ms exceeds 1.500ms; s2c relay p99 lateness 2.426ms exceeds 1.500ms; endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss50-candidate-default-s47-c8012704: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss50-candidate-default-s71-79c09968: INVALID_HARNESS / FAIL: c2s relay p99 lateness 3.585ms exceeds 1.500ms; endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss50-candidate-default-s101-4b8e853b: INVALID_HARNESS / FAIL: c2s relay p99 lateness 2.742ms exceeds 1.500ms; endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss70-candidate-default-s11-62f22a5c: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss70-candidate-default-s29-81b62ca3: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss70-candidate-default-s47-1fd8c6d1: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss70-candidate-default-s71-7dbbe6ff: INVALID_HARNESS / FAIL: c2s relay p99 lateness 3.105ms exceeds 1.500ms; endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')
- upload-loss70-candidate-default-s101-8dff916b: PROVISIONAL_UNCALIBRATED / FAIL: endpoint 0 exited 1: RuntimeError('production send failed: -1/16420'); endpoint 1 exited 1: RuntimeError('production receive failed / unhealthy stream'); RuntimeError('production send failed: -1/16420'); RuntimeError('production receive failed / unhealthy stream')

# Loss-stall compact results

raw native UDP2 transport; trace diagnostics separate from untraced comparison

56 trials; 56 integrity passes; 7 service failures; 0 timing invalid; 0 artifact invalid; 0 trace invalid.

Calibration and the failed trace smoke run are separate evidence; neither is hidden by successful main comparisons.

| Phase | Case | Baseline → candidate | Subset | Pairs | Goodput Mbit/s | RTT p95/p99 ms | Scheduled p95/p99 ms |
|---|---|---|---|---:|---|---|---|
| diagnostic-101 | impaired-upload-mixed | timer-q48 → fast-q48 | all_observed_diagnostic | 1 | 7.448 → 8.145 | 177.009/232.490 → 64.164/102.420 | 217.069/327.115 → 77.324/107.118 |
| diagnostic-101 | impaired-upload-mixed | timer-q48 → fast-q48 | artifact_intact_timing_valid_complete | 0 | unknown → unknown | unknown/unknown → unknown/unknown | unknown/unknown → unknown/unknown |
| diagnostic-101 | impaired-upload-mixed | timer-q64 → fast-q64 | all_observed_diagnostic | 1 | 7.852 → 8.486 | 220.020/265.771 → 79.188/120.281 | 238.882/345.556 → 96.562/133.455 |
| diagnostic-101 | impaired-upload-mixed | timer-q64 → fast-q64 | artifact_intact_timing_valid_complete | 0 | unknown → unknown | unknown/unknown → unknown/unknown | unknown/unknown → unknown/unknown |
| diagnostic-29 | impaired-upload-mixed | timer-q48 → fast-q48 | all_observed_diagnostic | 1 | 7.723 → 8.148 | 64.154/248.322 → 64.933/104.226 | 202.640/264.979 → 78.684/117.545 |
| diagnostic-29 | impaired-upload-mixed | timer-q48 → fast-q48 | artifact_intact_timing_valid_complete | 0 | unknown → unknown | unknown/unknown → unknown/unknown | unknown/unknown → unknown/unknown |
| diagnostic-29 | impaired-upload-mixed | timer-q64 → fast-q64 | all_observed_diagnostic | 1 | 8.024 → 8.530 | 82.332/257.354 → 78.926/100.590 | 224.094/280.603 → 92.181/111.574 |
| diagnostic-29 | impaired-upload-mixed | timer-q64 → fast-q64 | artifact_intact_timing_valid_complete | 0 | unknown → unknown | unknown/unknown → unknown/unknown | unknown/unknown → unknown/unknown |
| main | clean-upload-mixed | reference-q48 → candidate-q48 | all_observed_diagnostic | 5 | 8.216 → 8.208 | 48.396/50.338 → 48.280/49.928 | 63.080/64.027 → 63.058/64.025 |
| main | clean-upload-mixed | reference-q48 → candidate-q48 | artifact_intact_timing_valid_complete | 5 | 8.216 → 8.208 | 48.396/50.338 → 48.280/49.928 | 63.080/64.027 → 63.058/64.025 |
| main | clean-upload-mixed | reference-q64 → candidate-q64 | all_observed_diagnostic | 5 | 8.813 → 8.798 | 74.931/75.808 → 75.014/76.439 | 84.642/88.860 → 85.867/89.177 |
| main | clean-upload-mixed | reference-q64 → candidate-q64 | artifact_intact_timing_valid_complete | 5 | 8.813 → 8.798 | 74.931/75.808 → 75.014/76.439 | 84.642/88.860 → 85.867/89.177 |
| main | impaired-upload-mixed | reference-q48 → candidate-q48 | all_observed_diagnostic | 5 | 7.732 → 8.213 | 64.141/232.035 → 64.030/87.310 | 190.585/249.476 → 77.555/102.375 |
| main | impaired-upload-mixed | reference-q48 → candidate-q48 | artifact_intact_timing_valid_complete | 3 | 7.728 → 8.179 | 64.141/232.035 → 64.459/87.310 | 190.585/241.025 → 78.793/102.375 |
| main | impaired-upload-mixed | reference-q64 → candidate-q64 | all_observed_diagnostic | 5 | 8.048 → 8.497 | 78.742/246.935 → 80.034/104.588 | 215.914/262.950 → 93.475/117.893 |
| main | impaired-upload-mixed | reference-q64 → candidate-q64 | artifact_intact_timing_valid_complete | 4 | 8.037 → 8.495 | 78.686/246.750 → 80.645/105.628 | 220.334/260.709 → 93.518/123.327 |
| smoke | clean-upload-mixed | timer-q48 → fast-q48 | all_observed_diagnostic | 1 | 8.240 → 8.158 | 48.238/48.686 → 48.420/91.325 | 62.770/63.039 → 63.300/100.323 |
| smoke | clean-upload-mixed | timer-q48 → fast-q48 | artifact_intact_timing_valid_complete | 1 | 8.240 → 8.158 | 48.238/48.686 → 48.420/91.325 | 62.770/63.039 → 63.300/100.323 |
| smoke | impaired-upload-mixed | timer-q48 → fast-q48 | all_observed_diagnostic | 1 | 8.276 → 8.182 | 63.012/66.307 → 61.792/94.110 | 71.545/78.656 → 73.670/96.522 |
| smoke | impaired-upload-mixed | timer-q48 → fast-q48 | artifact_intact_timing_valid_complete | 1 | 8.276 → 8.182 | 63.012/66.307 → 61.792/94.110 | 71.545/78.656 → 73.670/96.522 |

## Failures retained

- main/impaired-upload-mixed-reference-q48-s101-98b7efeb: PROVISIONAL_UNCALIBRATED / FAIL; probe deadlines unserved/unobserved
- main/impaired-upload-mixed-reference-q48-s29-3bf6f00f: PROVISIONAL_UNCALIBRATED / FAIL; probe deadlines unserved/unobserved
- main/impaired-upload-mixed-reference-q64-s101-1cea3e75: PROVISIONAL_UNCALIBRATED / FAIL; probe deadlines unserved/unobserved
- diagnostic-29/impaired-upload-mixed-timer-q48-s29-557a0f14: PROVISIONAL_UNCALIBRATED / FAIL; probe deadlines unserved/unobserved
- diagnostic-29/impaired-upload-mixed-timer-q64-s29-98b92713: PROVISIONAL_UNCALIBRATED / FAIL; probe deadlines unserved/unobserved
- diagnostic-101/impaired-upload-mixed-timer-q48-s101-e41f27e7: PROVISIONAL_UNCALIBRATED / FAIL; probe deadlines unserved/unobserved
- diagnostic-101/impaired-upload-mixed-timer-q64-s101-88270174: PROVISIONAL_UNCALIBRATED / FAIL; probe deadlines unserved/unobserved

## calibration

```json
{
  "version": 1,
  "pass": false,
  "approved": false,
  "scope": "HARNESS_CALIBRATION_ONLY",
  "limitations": [
    "No approved calibration receipt is generated.",
    "Short samples check deterministic loss and delay envelopes; they do not tightly estimate a 0.05% population loss rate.",
    "Native peer/driver capacity and native instrumentation overhead require separate direct-socket evidence.",
    "Packet timestamps are local application observations on one Mac, not physical NIC timestamps."
  ],
  "target_headroom_pps": 6000,
  "empirically_passed_packet_rate": 0,
  "covers_2x_supplied_native_pps": false,
  "driver_sha256": "672df46e6179e4c7066a60ffedbf2c87f58cb5c084ca7bb127a43febc9aed378",
  "engine_files": {
    "rdpbench/link.py": "e67fbe8195a7161022ea0aee06db7402542080094534e230b939e3e2f59d6600",
    "rdpbench/common.py": "ae783ff60f8c758123a2c86059831f2caa574e9fcfa6cd84d9fdb6bdbdb14191"
  },
  "profile_source_sha256": "1a34ebb3af88e073efcb0200511bb5b85f61fd8b8040017ff4d4f09b92629df6",
  "available": true,
  "failed_records": [
    {
      "path": "",
      "pass": false
    },
    {
      "path": "samples[12]",
      "profile_id": "ten_mbit_clean",
      "mode": "paced",
      "direction": "s2c",
      "seed": 47,
      "pass": false,
      "errors": [],
      "evidence": {
        "path": "relay-calibration/ten_mbit_clean-paced-1-s47-e17e3134/result.json",
        "sha256": "712d5414fa3fd7ca50fd1475b97f6f836609328d0a4ab6ad925cc61f607951c3",
        "packet_evidence_path": "relay-calibration/ten_mbit_clean-paced-1-s47-e17e3134/packets.jsonl",
        "packet_evidence_sha256": "37d7d3e74bb5b0a46a7392c2b98884c180e5aec74e66441e480fb654630024b7"
      },
      "failed_criteria": [
        {
          "criterion": "no unintended paced queue loss",
          "pass": false,
          "detail": 37
        },
        {
          "criterion": "seeded loss at independent receiver",
          "pass": false,
          "detail": {
            "expected_missing_ids": [],
            "observed_missing_ids": [
              2441,
              2442,
              2443,
              2444,
              2445,
              2446,
              2447,
              2448,
              2449,
              2450,
              2451,
              2452,
              2453,
              2454,
              2455,
              2456,
              2457,
              2458,
              2459,
              2460,
              2461,
              2462,
              2463,
              2464,
              2465,
              2466,
              2467,
              2468,
              2469,
              2470,
              2471,
              2472,
              2474,
              2475,
              2476,
              2477,
              2478
            ]
          }
        },
        {
          "criterion": "paced one-way delay/jitter envelope",
          "pass": false,
          "detail": {
            "expected_base_ms": 16.008,
            "configured_uniform_jitter_ms": 0,
            "observed_ms": {
              "count": 4428,
              "p50": 16.226459,
              "p95": 26.0365,
              "p99": 43.181667,
              "max": 92.3425
            },
            "note": "Includes local socket, CRC driver and scheduling; not a statistical loss/jitter certification."
          }
        }
      ]
    },
    {
      "path": "samples[14]",
      "profile_id": "ten_mbit_clean",
      "mode": "saturated",
      "direction": "s2c",
      "seed": 47,
      "pass": false,
      "errors": [],
      "evidence": {
        "path": "relay-calibration/ten_mbit_clean-saturated-1-s47-f34b804c/result.json",
        "sha256": "05976eafb6a3dc4ffd3fdde51753bd2052ea980b1fff8f60d8f0c0b9fed8728f",
        "packet_evidence_path": "relay-calibration/ten_mbit_clean-saturated-1-s47-f34b804c/packets.jsonl",
        "packet_evidence_sha256": "de274e9e4f265382c50c29fa0d58a2265f7733223ec40cde81f6b53f66b03afa"
      },
      "failed_criteria": [
        {
          "criterion": "s2c relay dispatch timing",
          "pass": false,
          "detail": {
            "p99_ms": 1.57175,
            "budget_ms": 1.5
          }
        }
      ]
    },
    {
      "path": "samples[15]",
      "profile_id": "ten_mbit",
      "mode": "probe",
      "direction": "c2s",
      "seed": 11,
      "pass": false,
      "errors": [],
      "evidence": {
        "path": "relay-calibration/ten_mbit-probe-0-s11-fcfd968e/result.json",
        "sha256": "64e5eaf68dae46e8900eef309a7c109a50ef0228ba5590bce4990c7c492dc530",
        "packet_evidence_path": "relay-calibration/ten_mbit-probe-0-s11-fcfd968e/packets.jsonl",
        "packet_evidence_sha256": "835d0df38d3a40eb6090ef9efbadf000f3708e00c14649727f1431813f57ac7a"
      },
      "failed_criteria": [
        {
          "criterion": "unloaded raw RTT",
          "pass": false,
          "detail": {
            "expected_base_ms": 30.2048,
            "configured_total_jitter_ms": 2,
            "observed_ms": {
              "count": 300,
              "p50": 31.569083,
              "p95": 33.176958,
              "p99": 41.928875,
              "max": 72.176667
            }
          }
        }
      ]
    },
    {
      "path": "samples[16]",
      "profile_id": "ten_mbit",
      "mode": "paced",
      "direction": "c2s",
      "seed": 11,
      "pass": false,
      "errors": [],
      "evidence": {
        "path": "relay-calibration/ten_mbit-paced-0-s11-59acdf67/result.json",
        "sha256": "c27d5f628a6cdf5d72a38e4e4d7a51e1672bfc26c5181743a1e362838790d2bb",
        "packet_evidence_path": "relay-calibration/ten_mbit-paced-0-s11-59acdf67/packets.jsonl",
        "packet_evidence_sha256": "066ae2be8e35f9ba5ad250698ad33b09c1597841dd078286f2f69ce8a24685d3"
      },
      "failed_criteria": [
        {
          "criterion": "paced one-way delay/jitter envelope",
          "pass": false,
          "detail": {
            "expected_base_ms": 16.008,
            "configured_uniform_jitter_ms": 1,
            "observed_ms": {
              "count": 4463,
              "p50": 16.387375,
              "p95": 17.563458,
              "p99": 20.529625,
              "max": 23.128917
            },
            "note": "Includes local socket, CRC driver and scheduling; not a statistical loss/jitter certification."
          }
        }
      ]
    },
    {
      "path": "samples[20]",
      "profile_id": "ten_mbit",
      "mode": "probe",
      "direction": "c2s",
      "seed": 29,
      "pass": false,
      "errors": [],
      "evidence": {
        "path": "relay-calibration/ten_mbit-probe-0-s29-7a267768/result.json",
        "sha256": "ee27dc24951fee8ae3d5dd5d86af801b90baf18791415b8242452579b1771d30",
        "packet_evidence_path": "relay-calibration/ten_mbit-probe-0-s29-7a267768/packets.jsonl",
        "packet_evidence_sha256": "0659e70bdab0d282677e0a2fd3d2826959fc45c62247b230ad84b418c754c621"
      },
      "failed_criteria": [
        {
          "criterion": "c2s relay dispatch timing",
          "pass": false,
          "detail": {
            "p99_ms": 2.246814,
            "budget_ms": 1.5
          }
        },
        {
          "criterion": "s2c relay dispatch timing",
          "pass": false,
          "detail": {
            "p99_ms": 2.010308,
            "budget_ms": 1.5
          }
        },
        {
          "criterion": "unloaded raw RTT",
          "pass": false,
          "detail": {
            "expected_base_ms": 30.2048,
            "configured_total_jitter_ms": 2,
            "observed_ms": {
              "count": 299,
              "p50": 31.731375,
              "p95": 33.447458,
              "p99": 37.234959,
              "max": 40.278125
            }
          }
        }
      ]
    },
    {
      "path": "samples[25]",
      "profile_id": "ten_mbit",
      "mode": "probe",
      "direction": "c2s",
      "seed": 47,
      "pass": false,
      "errors": [],
      "evidence": {
        "path": "relay-calibration/ten_mbit-probe-0-s47-c4a881f4/result.json",
        "sha256": "f562b1aca0c7d94a2627368af790f3ff7e696cad8d2b67fab039d79ed70efa7f",
        "packet_evidence_path": "relay-calibration/ten_mbit-probe-0-s47-c4a881f4/packets.jsonl",
        "packet_evidence_sha256": "1f9865a3df4ff9e47608ccd18396565f470d5647b4defa89ff7cd665ccae1fef"
      },
      "failed_criteria": [
        {
          "criterion": "c2s relay dispatch timing",
          "pass": false,
          "detail": {
            "p99_ms": 2.245559,
            "budget_ms": 1.5
          }
        },
        {
          "criterion": "s2c relay dispatch timing",
          "pass": false,
          "detail": {
            "p99_ms": 1.513783,
            "budget_ms": 1.5
          }
        },
        {
          "criterion": "unloaded raw RTT",
          "pass": false,
          "detail": {
            "expected_base_ms": 30.2048,
            "configured_total_jitter_ms": 2,
            "observed_ms": {
              "count": 300,
              "p50": 31.585333,
              "p95": 33.347375,
              "p99": 46.619083,
              "max": 58.234958
            }
          }
        }
      ]
    },
    {
      "path": "samples[31]",
      "profile_id": "loopback",
      "mode": "headroom",
      "direction": "c2s",
      "seed": 11,
      "pass": false,
      "errors": [],
      "evidence": {
        "path": "relay-calibration/headroom-observer-1-s11-89d6c470/result.json",
        "sha256": "5e33b447695e9591c8c695971d5b049eae5f4fa9789ea728aa1657860f80a436",
        "packet_evidence_path": "relay-calibration/headroom-observer-1-s11-89d6c470/packets.jsonl",
        "packet_evidence_sha256": "d68ac265e2078f81f85497f090ee4173936ae1de44fda42b5dbeb8a580a4b1d8"
      },
      "failed_criteria": [
        {
          "criterion": "c2s relay dispatch timing",
          "pass": false,
          "detail": {
            "p99_ms": 0.817,
            "budget_ms": 0.5
          }
        }
      ]
    }
  ],
  "failed_sample_count": 7
}
```

## failed_trace_smoke_v2

```json
{
  "available": true,
  "pass": false,
  "artifact_status": "status manifest retained; exporter does not audit failed preflight artifacts",
  "phases": {
    "smoke": {
      "complete": true,
      "expected_trials": 4,
      "observed_trials": 4,
      "returncode": 1,
      "failed_trials": [
        "clean-upload-mixed-timer-q48-s11-5c0f0d9c",
        "clean-upload-mixed-fast-q48-s11-d8ff5fa9",
        "impaired-upload-mixed-timer-q48-s11-90101849",
        "impaired-upload-mixed-fast-q48-s11-25e47c78"
      ]
    }
  }
}
```

## Sparse causal evidence

- smoke/clean-upload-mixed-fast-q48-s11-e7f9edcf: trace valid=True; stalls={}; misses={}; 0 matched feedback/drop/retry chains.
- smoke/clean-upload-mixed-timer-q48-s11-9b9578f6: trace valid=True; stalls={}; misses={}; 0 matched feedback/drop/retry chains.
- smoke/impaired-upload-mixed-fast-q48-s11-1f03f2ea: trace valid=True; stalls={}; misses={}; 1 matched feedback/drop/retry chains.
- smoke/impaired-upload-mixed-timer-q48-s11-b7cd7e03: trace valid=True; stalls={"native_admission_timer_recovery": 1}; misses={}; 1 matched feedback/drop/retry chains.
- diagnostic-29/impaired-upload-mixed-fast-q48-s29-06e023a3: trace valid=True; stalls={}; misses={}; 14 matched feedback/drop/retry chains.
- diagnostic-29/impaired-upload-mixed-fast-q64-s29-701bce72: trace valid=True; stalls={}; misses={}; 15 matched feedback/drop/retry chains.
- diagnostic-29/impaired-upload-mixed-timer-q48-s29-557a0f14: trace valid=True; stalls={"native_admission_timer_recovery": 14}; misses={"native_admission_timer_recovery": 1}; 14 matched feedback/drop/retry chains.
- diagnostic-29/impaired-upload-mixed-timer-q64-s29-98b92713: trace valid=True; stalls={"native_admission_timer_recovery": 14}; misses={"native_admission_timer_recovery": 1}; 14 matched feedback/drop/retry chains.
- diagnostic-101/impaired-upload-mixed-fast-q48-s101-ca0c955f: trace valid=True; stalls={}; misses={}; 13 matched feedback/drop/retry chains.
- diagnostic-101/impaired-upload-mixed-fast-q64-s101-42dba3f8: trace valid=True; stalls={}; misses={}; 13 matched feedback/drop/retry chains.
- diagnostic-101/impaired-upload-mixed-timer-q48-s101-e41f27e7: trace valid=True; stalls={"native_admission_timer_recovery": 12, "relay_timing_confounded": 1}; misses={"native_admission_timer_recovery": 1}; 13 matched feedback/drop/retry chains.
- diagnostic-101/impaired-upload-mixed-timer-q64-s101-88270174: trace valid=True; stalls={"native_admission_timer_recovery": 11, "relay_timing_confounded": 1}; misses={"relay_timing_confounded": 1}; 13 matched feedback/drop/retry chains.
- overhead/clean-trace-on-fast-q48-s11-8738c042: trace valid=True; stalls={}; misses={}; 0 matched feedback/drop/retry chains.
- overhead/clean-trace-on-fast-q48-s29-142bdb70: trace valid=True; stalls={}; misses={}; 0 matched feedback/drop/retry chains.

## Limits

- All failures remain visible. Strict pairs require both timing-valid COMPLETE artifact-intact trials.
- Per-trial percentiles are summarized by medians; no percentile samples are pooled.
- Trace-disabled trials and old fixed peers have unknown causal evidence, not zero native events.
- Misses include warmup; p95/p99 probe samples use measurement-scheduled probes.
- Native timer recovery evidence requires admission blocking, a matched C→S DATA drop, ready gap feedback and timer retry; relay intervals overlapping that call are checked separately.
- This establishes an observed native wait after known feedback, not exclusive causation: old-peer ACK generation and unmeasured host/socket intervals remain unknown.
- Trace-on/off overhead is descriptive; no independent calibration receipt is implied.
- All observed diagnostics retain failed and timing-invalid runs; hash-invalid metrics are withheld in every subset.
- Strict pairs require matching phase, case, comparison key and seed, plus timing-valid COMPLETE integrity-verified trials and audited requested tracing.
- Paired N is also reported per metric; an absent metric never becomes zero or changes the matching seeds silently.
- Sparse proof timestamps are relative to each trial's recorded sparse_proof_epoch_ns. Duration fields retain their original units.
- Relay-confounded calls can still contain observed native timer recovery proof. The proof does not establish exclusive causation.

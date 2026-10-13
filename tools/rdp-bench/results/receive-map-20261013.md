# Receive-map Windows loss rerun

| Loss | Session | Observed seconds | Server FPS mean (N) | Retained channels / span | Terminal outcome |
|---:|---|---:|---:|---:|---|
| 25% | original_stop_on_first_exit_ramp | 19.113 | unavailable (0) | 3252 / 3262 | Missing channel gap; exit 147 |
| 50% | fresh_session_remaining_level | 67.184 | 5.392 (3) | 2076 / 2077 | Missing channel gap; exit 147 |
| 70% | fresh_session_remaining_level | 58.183 | 3.166 (3) | 2616 / 2619 | Missing channel gap; exit 147 |

All levels were exercised once. The original ramp stopped at25%; remaining levels used fresh sessions. No old256-slot collision or receive budget/allocation/oversize failure occurred.

FPS values are means of observed valid10-second server-counter windows at roughly20-second cadence; no full-phase FPS, physical-client FPS, payload goodput or input-to-display tail latency claim.

- Original ramp stops on 25% exit; 50% and70% are one fresh session each, separately labelled.
- All trials retain terminal failures; no attempt was restarted to obtain a better result.
- Server counters do not establish physical client FPS, application goodput or input-to-display tail latency.
- Ingress applies loss to all incoming IPv4; no rate cap; large packets add1ms delay and70%reorder.
- Kernel random loss and upstream coloured-noise RNG are unseeded; profile parameters are repeatable, individual logical losses are not.
- Primary observer uses paired PDH queries rather than upstream continuous10s sampling; report means cover only qualified observed10s windows, not full-phase averages.

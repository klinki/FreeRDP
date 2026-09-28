#!/usr/bin/env python3
"""Check recorded held events against the measured two-display geometry.

Use each event's raw position, not the concurrently sampled global mouse state,
which may already describe a newer event. Final-send coordinates are API
arguments; this analysis does not claim packet capture or server receipt.
"""

import json
from pathlib import Path
import re
import statistics

HERE = Path(__file__).resolve().parent
log = (HERE / "fixed-client.log").read_text()
convert = re.compile(
    r"DRAG_CONVERT ([\d.]+) age_ms=([\d.]+) wid=(\d+) state=(\d+) "
    r"raw=(-?[\d.]+),(-?[\d.]+)"
)
final = re.compile(
    r"DRAG_FINAL ([\d.]+) age_ms=([\d.]+) wid=(\d+) state=(\d+) "
    r"relative=(\d+) x=(-?\d+) y=(-?\d+) global=(-?[\d.]+),(-?[\d.]+)"
)
button = re.compile(
    r"DRAG_BUTTON ([\d.]+) wid=(\d+) down=(\d+) button=(\d+) "
    r"relative=(\d+) x=(-?\d+) y=(-?\d+) global=(-?[\d.]+),(-?[\d.]+)"
)
events = sorted(
    [(m.start(), "raw", list(map(float, m.groups()))) for m in convert.finditer(log)]
    + [(m.start(), "final", list(map(float, m.groups()))) for m in final.finditer(log)]
)
rows = []
raw = None
for _, kind, values in events:
    if kind == "raw":
        raw = values
        continue
    t, age, wid, state, relative, x, y, gx, gy = values
    if state != 1:
        continue
    assert raw is not None and raw[2] == wid and raw[3] == state
    assert 0 <= t - raw[0] < 0.1 and relative == 0
    event_x = raw[4] + (1920 if wid == 7 else 0)
    event_y = raw[5] - 1080
    assert wid in (7, 8) and 0 <= event_x < 3840 and -1080 <= event_y < 0
    expected = (
        (event_x, event_y + 2160)
        if event_x < 1920
        else (2 * event_x - 1920, 2 * (event_y + 1080))
    )
    error = max(abs(x - expected[0]), abs(y - expected[1]))
    # Raw coordinates have 0.01-pixel log precision; the send API truncates.
    assert error < 1.02, (t, x, y, expected, error)
    rows.append(dict(t=t, age_ms=age, wid=wid, x=x, y=y, error_px=error))

buttons = [list(map(float, m.groups())) for m in button.finditer(log)]
summary = {}
for name, source, destination in (
    ("dell", 8, [3184, 538]),
    ("m27", 7, [1100, 1349]),
):
    for mode in ("roundtrip", "release"):
        helper = (HERE / f"fixed-{name}-{mode}.log").read_text()
        times = list(map(float, re.findall(r"^(\d+\.\d+)", helper, re.M)))
        assert len(times) == 4 and helper.count("down=true") >= 2
        selected = [r for r in rows if times[0] <= r["t"] <= times[-1]]
        assert selected and {r["wid"] for r in selected} == {source}
        endpoint = [r for r in selected if r["t"] <= times[1]][-1]
        assert [endpoint["x"], endpoint["y"]] == destination
        releases = [b for b in buttons if times[0] <= b[0] <= times[-1] + 0.2 and b[2] == 0]
        assert len(releases) == 1
        if mode == "release":
            assert releases[0][5:7] == destination
        ages = sorted(r["age_ms"] for r in selected)
        summary[f"{name}-{mode}"] = dict(
            events=len(selected),
            source_window=source,
            hold_endpoint=[endpoint["x"], endpoint["y"]],
            release=releases[0][5:7],
            max_error_px=max(r["error_px"] for r in selected),
            age_median_ms=statistics.median(ages),
            age_p95_ms=ages[int(0.95 * (len(ages) - 1))],
            age_max_ms=max(ages),
        )

(HERE / "analysis.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))

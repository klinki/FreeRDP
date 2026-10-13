# Flight/loss dashboard

Buildless dashboard assets for `flight_report.py` output. Serve over HTTP so the
browser can load `data.json`. The measured matrix varies outstanding reliable
DATA packets, not packet length.

Copy these three static assets to a new output folder, copy the audited
`dashboard.json` there as `data.json`, and copy `trials.csv` alongside it. Then:

```sh
python3 -m http.server 8868 --bind 127.0.0.1 --directory /absolute/dashboard/path
```

Open `http://127.0.0.1:8868/`. Diagnostic observations are the explicit default;
qualified and intact/timing-valid cohorts remain selectable. Incomplete streams
have no verified goodput; response counts and failures accompany survivor tails.
The JS/CSS query fingerprints must be refreshed when those asset bytes change.

The completed 13 October campaign is documented in
`../docs/FLIGHT-RESULTS-20261013.md` and `../results/flight-matrix-20261013.json`.

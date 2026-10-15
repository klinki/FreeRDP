# Graphics batch — 2026-10-15

The initial audit exercised 31 additional graphics cases in both modes. Sixteen
positive cases passed in both modes: H.264 support and 12 AVC420/AVC444/AVC444v2
stream variants, compression, and two frame-segmentation cases. These verify
negotiation and frame acknowledgements; rendered pixels are not compared.

The separated-chroma AVC444 Main-profile stream failed in both modes because
frame 2 was not acknowledged. The exact cause remains to be investigated;
[evidence](evidence/) includes the original client and Microsoft logs.

All 14 graphics-negative candidates are excluded. None passed both modes, and
some nominal standard-mode passes relied on Microsoft's defective disconnect
verifier. The [drive investigation](../drive/README.md) repaired that verifier
and confirmed both GFX capability negatives fail in both modes: the required
connection-close event was not observed. No assertion was relaxed to accept
channel-only rejection or cleanup termination.

[Initial results](qualification-initial.json) preserve all outcomes, including
the invalid nominal passes. Final enabled membership and combined patched-driver
qualification are recorded in [the batch report](../README.md).

For an independent source-driven audit, set `WPTS_SOURCE` to the pinned Microsoft
checkout and run `python3 probe.py /path/to/new/results`. The helper selects
methods not currently in extended, so after integration it probes the remaining
excluded methods. The current prepared suite must be built with `prepare.py`.

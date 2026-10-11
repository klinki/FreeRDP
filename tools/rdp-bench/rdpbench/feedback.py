"""Controlled ACK-only feedback delay; this does not model receiver ACK batching.

Classification reverses the production UDP2 prefix protection and validates the
complete visible layout. It never decrypts DATA, edits bytes, synthesizes ACKs,
or infers an ACK from packet length. Unknown/malformed packets are left alone.
"""
from __future__ import annotations

import collections
import math

from .common import timing_summary

ACK, DATA, ACKVEC, AOA, OVERHEAD, DELAYACK = 0x001, 0x004, 0x008, 0x010, 0x040, 0x100
KNOWN_FLAGS = ACK | DATA | ACKVEC | AOA | OVERHEAD | DELAYACK
KINDS = ("ack_only", "ackvec_only", "data_with_ack", "data_without_ack", "control", "dummy", "unclassified")
SELECTED_KINDS = ("ack_only", "ackvec_only")


def validate_feedback(value):
    if value is None:
        return None
    if not isinstance(value, dict) or set(value) != {"c2s_delay_ms", "s2c_delay_ms"}:
        raise ValueError("ack_feedback requires c2s_delay_ms and s2c_delay_ms only")
    for name, delay in value.items():
        if isinstance(delay, bool) or not isinstance(delay, (int, float)) or not math.isfinite(delay) or not 0 <= delay <= 1000:
            raise ValueError(f"invalid ack_feedback.{name}")
    return dict(value)


def packet_kind(data):
    """Classify a protected UDP2 datagram without changing the supplied bytes."""
    if len(data) < 8:
        return "unclassified"
    prefix = data[7]  # protect() swaps the prefix and byte seven.
    packet_type = (prefix >> 1) & 15
    short_length = prefix >> 5
    if prefix & 1 or packet_type not in (0, 8):
        return "unclassified"
    restored = bytearray(data)
    restored[0], restored[7] = restored[7], restored[0]
    if short_length < 7:
        if len(data) != 8 or any(restored[1 + short_length:]):
            return "unclassified"
        layout = restored[1:1 + short_length]
    else:
        layout = restored[1:]
    if len(layout) < 2:
        return "unclassified"
    flags = int.from_bytes(layout[:2], "little") & 0xFFF
    if flags & ~KNOWN_FLAGS or flags & ACK and flags & ACKVEC:
        return "unclassified"
    cursor = 2

    def take(size):
        nonlocal cursor
        if cursor + size > len(layout):
            return False
        cursor += size
        return True

    if flags & ACK:
        if cursor + 7 > len(layout):
            return "unclassified"
        if not take(7 + (layout[cursor + 6] & 15)):
            return "unclassified"
    for flag, size in ((OVERHEAD, 1), (DELAYACK, 3), (AOA, 2), (DATA, 2)):
        if flags & flag and not take(size):
            return "unclassified"
    if flags & ACKVEC:
        if cursor + 3 > len(layout):
            return "unclassified"
        vector = layout[cursor + 2]
        encoded_start = cursor + 3 + (4 if vector & 0x80 else 0)
        if not take(3 + (4 if vector & 0x80 else 0) + (vector & 0x7F)):
            return "unclassified"
        # Match production ACKVEC decoding: bitmap bytes cover seven packets;
        # RLE bytes cover their low-six-bit run. Empty/oversized coverage is
        # invalid even when the encoded layout fits inside the datagram.
        decoded = sum((byte & 0x3F) if byte & 0x80 else 7
                      for byte in layout[encoded_start:cursor])
        if not 0 < decoded <= 1024:
            return "unclassified"
    if flags & DATA:
        if not take(2):
            return "unclassified"
    elif cursor != len(layout):
        return "unclassified"
    if packet_type == 8:
        return "dummy"
    if flags & DATA:
        return "data_with_ack" if flags & (ACK | ACKVEC) else "data_without_ack"
    if flags & ACK:
        return "ack_only"
    if flags & ACKVEC:
        return "ackvec_only"
    return "control"


class FeedbackTiming:
    """Keep selected-delivery timing separate from network deadline lateness."""
    CAPACITY = 200000

    def __init__(self, delay_ms):
        self.delay_ms = delay_ms
        self.delay_ns = int(delay_ms * 1e6)
        self.classification = collections.Counter()
        self.scheduled = 0
        self.delivered = 0
        self.added = []
        self.overshoot = []
        self.added_max = None
        self.overshoot_max = None
        self.records = []
        self.ledger_overflow = 0

    def classify(self, data):
        kind = packet_kind(data)
        self.classification[kind] += 1
        return kind

    def schedule(self, base_due):
        self.scheduled += 1
        return base_due + self.delay_ns

    def observe(self, base_due, actual, sample_index, udp_bytes=None, kind=None):
        self.delivered += 1
        added = max(0, actual - base_due) / 1e6
        overshoot = max(0, actual - base_due - self.delay_ns) / 1e6
        self.added_max = max(self.added_max or 0, added)
        self.overshoot_max = max(self.overshoot_max or 0, overshoot)
        if len(self.records) < self.CAPACITY:
            self.records.append({"kind": kind, "udp_bytes": udp_bytes,
                                 "network_due_ns": base_due,
                                 "feedback_due_ns": base_due + self.delay_ns,
                                 "actual_dispatch_ns": actual})
        else:
            self.ledger_overflow += 1
        if len(self.added) < self.CAPACITY:
            self.added.append(added)
            self.overshoot.append(overshoot)
        elif sample_index < self.CAPACITY:
            self.added[sample_index] = added
            self.overshoot[sample_index] = overshoot

    def summary(self):
        def timings(samples, maximum):
            result = timing_summary(samples)
            result.update(max=maximum, sample_count=len(samples), delivered_count=self.delivered,
                          sampled=self.delivered > len(samples))
            return result
        return {"version": 1, "scope": "relay ACK/ACKVEC-only deadline offset, not receiver ACK batching",
                "requested_delay_ms": self.delay_ms,
                "classification": {kind: self.classification[kind] for kind in KINDS},
                "classified_datagrams": sum(self.classification.values()),
                "selected_datagrams": sum(self.classification[kind] for kind in SELECTED_KINDS),
                "scheduled_datagrams": self.scheduled, "delivered_datagrams": self.delivered,
                "ledger_events": len(self.records), "ledger_overflow_datagrams": self.ledger_overflow,
                "added_delivery_ms": timings(self.added, self.added_max),
                "scheduler_overshoot_ms": timings(self.overshoot, self.overshoot_max),
                "reservoir_capacity": self.CAPACITY,
                "notes": ["Added delay is actual relay dispatch minus the unmodified network delivery deadline.",
                          "It includes the requested offset and scheduler overshoot; it is not endpoint ACK latency.",
                          "DATA with piggybacked ACKs, control, dummy, malformed and unknown packets receive no offset."]}

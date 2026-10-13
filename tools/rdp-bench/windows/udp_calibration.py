#!/usr/bin/env python3
"""Disposable CRC UDP echo receiver for VM relay calibration; this is not RDP.

The caller owns the output directory and stop file. No session or network
configuration is changed by this program. Windows monotonic timestamps remain
in their own clock domain; the host computes RTT from its echoed timestamps.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
from pathlib import Path
import socket
import struct
import time
import zlib

MAGIC = b"RDCAL1\0\0"
HEADER = struct.Struct("!8s16sIQI")
CAPACITY = 100000


def decode_packet(data):
    if len(data) < HEADER.size:
        return None
    magic, sample, seq, sent_ns, crc = HEADER.unpack_from(data)
    if magic != MAGIC:
        return None
    valid = zlib.crc32(data[:HEADER.size-4]+data[HEADER.size:]) == crc
    return dict(sample_id=sample.hex(), seq=seq, sent_ns=sent_ns,
                udp_bytes=len(data), crc_valid=valid)


def write_json(path, value):
    temporary = Path(str(path)+".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    temporary.replace(path)


def run(args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    begin = time.monotonic_ns()
    cpu_begin = time.process_time_ns()
    records = []
    overflow = 0
    counts = collections.Counter()
    seen = collections.defaultdict(set)
    maximum = collections.defaultdict(lambda: -1)
    state, error = "lease_expired", None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as value:
        value.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
        value.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4*1024*1024)
        value.bind((args.bind, args.port))
        value.settimeout(.1)
        write_json(output/"ready.json", dict(run_id=args.run_id, pid=os.getpid(),
                   bind=list(value.getsockname()), state="active", started_ns=begin,
                   clock="Windows monotonic_ns; host alignment unknown", capacity=CAPACITY))
        try:
            while time.monotonic_ns()-begin < args.lease_s*1e9:
                if (output/"stop").exists():
                    state = "owned_stop_file"
                    break
                try:
                    data, peer = value.recvfrom(65535)
                except socket.timeout:
                    continue
                received_ns = time.monotonic_ns()
                counts["received_datagrams"] += 1
                counts["received_udp_bytes"] += len(data)
                row = decode_packet(data)
                if row is None:
                    counts["unrecognized_datagrams"] += 1
                    continue
                row.update(received_time_ns=received_ns, source=list(peer))
                if not row["crc_valid"]:
                    counts["crc_failures"] += 1
                else:
                    sample, seq = row["sample_id"], row["seq"]
                    if seq in seen[sample]:
                        counts["duplicates"] += 1
                    elif seq < maximum[sample]:
                        counts["reordered"] += 1
                    seen[sample].add(seq)
                    maximum[sample] = max(maximum[sample], seq)
                    sent = value.sendto(data, peer)
                    if sent != len(data):
                        raise RuntimeError("partial UDP datagram echo")
                    row["echoed_time_ns"] = time.monotonic_ns()
                    counts["echoed_datagrams"] += 1
                    counts["echoed_udp_bytes"] += sent
                if len(records) < CAPACITY:
                    records.append(row)
                else:
                    overflow += 1
        except BaseException as failure:
            state, error = "failed", type(failure).__name__+": "+str(failure)
    with (output/"receiver.jsonl").open("x", encoding="utf-8", newline="\n") as stream:
        for row in records:
            stream.write(json.dumps(row, separators=(",", ":"))+"\n")
    result = dict(run_id=args.run_id, pid=os.getpid(), state=state, error=error,
                  sockets_closed=True, started_ns=begin, ended_ns=time.monotonic_ns(),
                  cpu_s=(time.process_time_ns()-cpu_begin)/1e9, counts=dict(counts),
                  overflow=overflow, record_count=len(records), capacity=CAPACITY,
                  clock="Windows monotonic_ns; host alignment unknown", scope="CRC UDP echo, not RDP",
                  valid=not error and not overflow and not counts["crc_failures"])
    write_json(output/"summary.json", result)
    return 0 if result["valid"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=63389)
    parser.add_argument("--lease-s", type=float, default=240)
    args = parser.parse_args()
    if not 0 < args.lease_s <= 3600 or not 1 <= args.port <= 65535:
        parser.error("port or lease is out of bounds")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())

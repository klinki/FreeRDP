#!/usr/bin/env python3
"""Audit a leased live relay against a separate CRC UDP echo receiver.

This measures raw UDP echo service, never RDP application goodput. Receiver
timestamps remain in the remote clock domain. The caller starts and cleans up
the disposable receiver; this driver only owns its relay leases and sockets.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
from pathlib import Path
import select
import socket
import struct
import sys
import time
import uuid
import zlib

from live_relay import adapter, encoded
from rdpbench.common import environment, read_json, rows, sha256, timing_summary, write_json
from rdpbench.link import Link
from windows.udp_calibration import HEADER, MAGIC, decode_packet


def packet(sample, seq, sent_ns, size, seed):
    body = hashlib.sha256(str(seed).encode()).digest()
    body = (body*((size-HEADER.size+31)//32))[:size-HEADER.size]
    prefix = struct.pack("!8s16sIQ", MAGIC, sample.bytes, seq, sent_ns)
    return prefix+struct.pack("!I", zlib.crc32(prefix+body))+body


def run_sample(args, output, profile_id, profile, seed, mode):
    folder = output/(profile_id+"-"+mode+"-s"+str(seed))
    folder.mkdir(exist_ok=False)
    sample_id = uuid.uuid4()
    run_id = "vm-udp-calibration-"+sample_id.hex
    proxy = argparse.Namespace(state=str(output/"relay-state.json"), output_dir=str(folder),
                              listen_host="127.0.0.1", listen_port=args.listen_port,
                              target_host=args.target_host, target_port=args.target_port,
                              request=encoded(dict(op="apply", run_id=run_id, profile_id=profile_id,
                                                   profile=profile, seed=seed, lease_s=args.seconds+20,
                                                   trial_dir=str(folder))))
    prepared = adapter(proxy)
    write_json(folder/"relay-prepared.json", prepared)
    size = args.packet_bytes if mode == "paced" else 64
    pps = args.paced_mbps*1e6/((size+28)*8) if mode == "paced" else args.probe_hz
    interval_ns = int(1e9/pps)
    sent, echoed = {}, {}
    records, deadline_lags = [], []
    counts = collections.Counter()
    error = None
    begin_ns = time.monotonic_ns()
    end_ns = begin_ns+int(args.seconds*1e9)
    cpu_begin = time.process_time_ns()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as value:
            value.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
            value.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4*1024*1024)
            value.bind(("127.0.0.1", 0))
            value.setblocking(False)
            seq, maximum_echo = 0, -1
            next_ns = begin_ns
            while time.monotonic_ns() < end_ns+int(args.drain_s*1e9):
                now = time.monotonic_ns()
                if next_ns < end_ns and now >= next_ns:
                    data = packet(sample_id, seq, now, size, seed)
                    written = value.sendto(data, ("127.0.0.1", args.listen_port))
                    if written != size:
                        raise RuntimeError("partial UDP datagram send")
                    sent[seq] = data
                    deadline_lags.append((now-next_ns)/1e6)
                    records.append(dict(type="send", seq=seq, due_ns=next_ns, sent_ns=now,
                                        udp_bytes=size, crc_valid=True))
                    counts["sent_datagrams"] += 1
                    counts["sent_udp_bytes"] += size
                    seq += 1
                    next_ns = begin_ns+seq*interval_ns
                timeout = max(0, min(.005, ((next_ns if next_ns < end_ns else end_ns+int(args.drain_s*1e9))-time.monotonic_ns())/1e9))
                readable, _, _ = select.select([value], [], [], timeout)
                if not readable:
                    continue
                for _ in range(64):
                    try:
                        data, source = value.recvfrom(65535)
                    except BlockingIOError:
                        break
                    received_ns = time.monotonic_ns()
                    row = decode_packet(data)
                    counts["received_datagrams"] += 1
                    counts["received_udp_bytes"] += len(data)
                    if source != ("127.0.0.1", args.listen_port):
                        counts["unexpected_source"] += 1
                    if row is None or not row["crc_valid"] or row["sample_id"] != sample_id.hex:
                        counts["corrupt_or_unexpected_datagrams"] += 1
                        continue
                    sequence = row["seq"]
                    if data != sent.get(sequence):
                        counts["payload_mismatch"] += 1
                        continue
                    if sequence in echoed:
                        counts["duplicates"] += 1
                    else:
                        if sequence < maximum_echo:
                            counts["reordered"] += 1
                        echoed[sequence] = received_ns
                        maximum_echo = max(maximum_echo, sequence)
                        if begin_ns <= received_ns < end_ns:
                            counts["measurement_echo_udp_bytes"] += len(data)
                    records.append(dict(type="echo", seq=sequence, sent_ns=row["sent_ns"],
                                        received_ns=received_ns, udp_bytes=len(data), crc_valid=True,
                                        rtt_ms=(received_ns-row["sent_ns"])/1e6))
    except BaseException as failure:
        error = type(failure).__name__+": "+str(failure)
    finally:
        proxy.request = encoded(dict(op="restore", run_id=run_id, profile_id=profile_id))
        restored = adapter(proxy)
        write_json(folder/"relay-restored.json", restored)
    with (folder/"host.jsonl").open("x", encoding="utf-8", newline="\n") as stream:
        for row in records:
            stream.write(json.dumps(row, separators=(",", ":"))+"\n")
    result = dict(sample_id=sample_id.hex, run_id=run_id, profile_id=profile_id,
                  profile=profile, seed=seed, mode=mode, seconds=args.seconds,
                  packet_bytes=size, requested_wire_mbps=args.paced_mbps if mode == "paced" else None,
                  begin_ns=begin_ns, end_ns=end_ns, drain_s=args.drain_s, counts=dict(counts),
                  sender_wire_mbps=len(sent)*(size+28)*8/args.seconds/1e6,
                  verified_echo_payload_mbps=len(echoed)*size*8/args.seconds/1e6,
                  measurement_echo_payload_mbps=counts["measurement_echo_udp_bytes"]*8/args.seconds/1e6,
                  measurement_echo_wire_mbps=(counts["measurement_echo_udp_bytes"]//size)*(size+28)*8/args.seconds/1e6,
                  rtt_ms=timing_summary([(echoed[seq]-HEADER.unpack_from(sent[seq])[3])/1e6 for seq in echoed]),
                  send_deadline_lateness_ms=timing_summary(deadline_lags),
                  host_cpu_s=(time.process_time_ns()-cpu_begin)/1e9, error=error,
                  missing_echo_sequences=sorted(set(sent)-set(echoed)),
                  evidence=str(folder.relative_to(output)),
                  relay_ledger=str(Path(restored["capturepath"]).relative_to(output)),
                  scope="CRC UDP echo calibration, not RDP goodput", accepted_calibration_receipt=False,
                  clocks=dict(host="Mac monotonic_ns", receiver="Windows monotonic_ns; alignment unknown"))
    write_json(folder/"host-summary.json", result)
    print(json.dumps(dict(sample=result["evidence"], sent=len(sent), echoes=len(echoed),
                         rtt_p95_ms=result["rtt_ms"]["p95"], rtt_p99_ms=result["rtt_ms"]["p99"],
                         missing=len(result["missing_echo_sequences"]), error=error)), flush=True)
    return result


def criterion(result, name, passed, detail):
    result["criteria"].append(dict(criterion=name, **{"pass": bool(passed)}, detail=detail))


def finalize(output, receiver_jsonl, receiver_summary):
    host = read_json(output/"host-campaign.json")
    remote = read_json(receiver_summary)
    by_sample = collections.defaultdict(list)
    for row in rows(receiver_jsonl):
        by_sample[row["sample_id"]].append(row)
    for sample in host["samples"]:
        folder = output/sample["evidence"]
        relay = read_json(folder/"relay-restored.json")
        ledger = list(rows(output/sample["relay_ledger"]))
        received = by_sample[sample["sample_id"]]
        counts = sample["counts"]
        directions = relay["directions"]
        sample["criteria"] = []
        sample["receiver_counts"] = dict(datagrams=len(received), unique=len({r["seq"] for r in received}),
                                         crc_failures=sum(not r["crc_valid"] for r in received),
                                         echoed=sum("echoed_time_ns" in r for r in received))
        host_sends = {r["seq"]: r for r in rows(folder/"host.jsonl") if r["type"] == "send"}
        remote_bad_ids = [r["seq"] for r in received if r["seq"] not in host_sends
                          or r["sent_ns"] != host_sends[r["seq"]]["sent_ns"]
                          or r["udp_bytes"] != sample["packet_bytes"]]
        sample["receiver_missing_sequences"] = sorted(set(host_sends)-{r["seq"] for r in received})
        sample["receiver_echo_service_ms"] = timing_summary([(r["echoed_time_ns"]-r["received_time_ns"])/1e6
                                                           for r in received if "echoed_time_ns" in r])
        sample["relay"] = {key: relay[key] for key in ("valid", "state", "sockets_closed", "ledger", "directions", "udp_errors", "tcp_connections")}
        bad = [key for key in ("corrupt_or_unexpected_datagrams", "payload_mismatch", "duplicates", "unexpected_source") if counts.get(key)]
        criterion(sample, "CRC and exact echoed payload integrity", not bad and not sample["receiver_counts"]["crc_failures"],
                  dict(host_failures=bad, receiver=sample["receiver_counts"]))
        criterion(sample, "VM received sequence/timestamp/length identity", not remote_bad_ids
                  and sample["receiver_counts"]["unique"] == len(received),
                  dict(mismatched_ids=remote_bad_ids, missing_sequences=sample["receiver_missing_sequences"],
                       note="Missing packets are accounted by explicit forward relay drops; jitter may reorder arrival."))
        criterion(sample, "owned relay closed and valid ledger", relay["valid"] and relay["sockets_closed"]
                  and relay["ledger"]["closed"] and relay["ledger"]["valid"] and not relay["error"], relay["ledger"])
        criterion(sample, "socket/runner/receiver errors", sample["error"] is None and remote["valid"]
                  and not relay["udp_errors"] and not any(d.get("socket_send_errors") for d in directions.values()),
                  dict(host=sample["error"], receiver=remote["error"], relay=relay["udp_errors"]))
        c2s, s2c = directions["c2s"], directions["s2c"]
        criterion(sample, "host→relay→VM→relay→host datagram accounting",
                  counts["sent_datagrams"] == c2s["received_datagrams"]
                  and c2s["delivered_datagrams"] == len(received)
                  and sample["receiver_counts"]["echoed"] == s2c["received_datagrams"]
                  and s2c["delivered_datagrams"] == counts["received_datagrams"]
                  and all(d["packet_accounting"]["balanced"] and not d["pending_datagrams"] for d in directions.values()),
                  dict(host_sent=counts["sent_datagrams"], forward=c2s["packet_accounting"], vm_received=len(received),
                       vm_echoed=sample["receiver_counts"]["echoed"], reverse=s2c["packet_accounting"], host_echoes=counts["received_datagrams"]))
        remote_bytes = sum(r["udp_bytes"] for r in received)
        echo_bytes = sum(r["udp_bytes"] for r in received if "echoed_time_ns" in r)
        criterion(sample, "host→relay→VM→relay→host UDP byte accounting",
                  counts["sent_udp_bytes"] == c2s["received_udp_bytes"]
                  and c2s["delivered_udp_bytes"] == remote_bytes
                  and echo_bytes == s2c["received_udp_bytes"]
                  and s2c["delivered_udp_bytes"] == counts["received_udp_bytes"],
                  dict(host_sent=counts["sent_udp_bytes"], relay_forward_received=c2s["received_udp_bytes"],
                       relay_forward_delivered=c2s["delivered_udp_bytes"], vm_received=remote_bytes,
                       vm_echoed=echo_bytes, relay_reverse_received=s2c["received_udp_bytes"],
                       relay_reverse_delivered=s2c["delivered_udp_bytes"], host_received=counts["received_udp_bytes"]))
        for name, direction in directions.items():
            offers = [r for r in ledger if r["type"] == "offer" and r["direction"] == name]
            dispatch = [r for r in ledger if r["type"] == "dispatch" and r["direction"] == name]
            drops = [r for r in ledger if r["type"] == "drop" and r["direction"] == name]
            check = Link(sample["profile"][name], sample["seed"]+(1000003 if name == "s2c" else 0), name)
            check.start_ns = relay["started_ns"]
            expected = {}
            for row in offers:
                before = dict(check.count)
                planned = check.schedule(row["offered_ns"], bytes(row["udp_bytes"]))
                reason = next((key for key in ("blackout_drops", "queue_drops", "random_drops")
                               if check.count[key] > before.get(key, 0)), None)
                expected[row["packet_id"]] = (planned, reason)
            mismatch = []
            for row in drops:
                if expected[row["packet_id"]][1] != row["reason"]:
                    mismatch.append(row["packet_id"])
            for row in dispatch:
                if row["due_ns"] not in [p[0] for p in expected[row["packet_id"]][0]] or row["actual_dispatch_ns"] < row["due_ns"] or row["lateness_ns"] != row["actual_dispatch_ns"]-row["due_ns"]:
                    mismatch.append(row["packet_id"])
            criterion(sample, name+" seeded schedule/deadline/drop ledger audit", not mismatch
                      and len(offers) == direction["received_datagrams"] and len(dispatch) == direction["delivered_datagrams"]
                      and len(drops) == sum(direction.get(k, 0) for k in ("blackout_drops", "queue_drops", "random_drops")),
                      dict(mismatched_packet_ids=mismatch, offers=len(offers), dispatches=len(dispatch), drops=len(drops)))
            budget = max(.5, sample["profile"][name]["one_way_ms"]*.1)
            lag = direction["lateness_ms"]["p99"]
            criterion(sample, name+" relay p99 dispatch lateness", lag is not None and lag <= budget,
                      dict(p99_ms=lag, budget_ms=budget, max_ms=direction["lateness_ms"]["max"]))
            criterion(sample, name+" no unintended queue drops", not direction["queue_drops"], direction["queue_drops"])
        if sample["mode"] == "paced":
            target = sample["requested_wire_mbps"]
            criterion(sample, "sender offered IPv4+UDP wire rate", .95*target <= sample["sender_wire_mbps"] <= 1.05*target,
                      dict(requested_mbps=target, measured_mbps=sample["sender_wire_mbps"], tolerance_fraction=.05))
            criterion(sample, "CRC verified measurement echo wire rate", .95*target <= sample["measurement_echo_wire_mbps"] <= 1.05*target,
                      dict(requested_mbps=target, measured_mbps=sample["measurement_echo_wire_mbps"], tolerance_fraction=.05,
                           note="Round trip UDP echoes; both directions paced, includes startup RTT, not RDP goodput"))
        else:
            base = sum(sample["profile"][d]["one_way_ms"]+(sample["packet_bytes"]+28)*8/(sample["profile"][d]["rate_mbps"]*1000) for d in ("c2s", "s2c"))
            jitter = sum(sample["profile"][d]["jitter_ms"] for d in ("c2s", "s2c"))
            latency = sample["rtt_ms"]
            criterion(sample, "unloaded RTT envelope including VM echo service", latency["p50"] is not None
                      and base-jitter-1 <= latency["p50"] <= base+jitter+3 and latency["p99"] <= base+jitter+5,
                      dict(expected_base_ms=base, jitter_ms=jitter, measured_ms=latency,
                           note="No subtraction of Windows echo scheduling or unaligned one-way timestamps"))
        sample["pass"] = all(c["pass"] for c in sample["criteria"])
        sample["evidence_sha256"] = {str(p.relative_to(folder)): sha256(p) for p in folder.rglob("*") if p.is_file()}
    host.update(receiver_summary=remote, receiver_jsonl_sha256=sha256(receiver_jsonl),
                samples_passed=sum(s["pass"] for s in host["samples"]), sample_count=len(host["samples"]),
                **{"pass": all(s["pass"] for s in host["samples"])},
                accepted_calibration_receipt=False,
                limitations=["Raw UDP echo is not RDP application goodput or protocol latency.",
                             "Windows and Mac monotonic clocks are unaligned; one-way VM latency is unknown.",
                             "Short seeded samples audit exact model decisions, not a population estimate of 0.05% loss.",
                             "No saturation, 2× packet-rate headroom or observer-overhead certification was performed."])
    write_json(output/"calibration.json", host)
    report = ["# Actual VM UDP relay calibration", "", "Raw CRC UDP echo, **not RDP goodput**. No accepted calibration receipt is issued.", "",
              "| Profile | Mode | Seed | Sent / echoed | Verified echo Mbit/s | RTT p95 / p99 ms | Status |", "|---|---|---:|---:|---:|---:|---|"]
    for sample in host["samples"]:
        report.append("| {profile_id} | {mode} | {seed} | {sent}/{echo} | {rate:.3f} | {p95}/{p99} | {status} |".format(
            **sample, sent=sample["counts"]["sent_datagrams"], echo=sample["counts"]["received_datagrams"],
            rate=sample["verified_echo_payload_mbps"], p95="unknown" if sample["rtt_ms"]["p95"] is None else f'{sample["rtt_ms"]["p95"]:.3f}',
            p99="unknown" if sample["rtt_ms"]["p99"] is None else f'{sample["rtt_ms"]["p99"]:.3f}', status="PASS" if sample["pass"] else "FAIL"))
    report.extend(["", "Failures:", ""])
    for sample in host["samples"]:
        for check in sample["criteria"]:
            if not check["pass"]:
                report.append("- "+sample["evidence"]+": "+check["criterion"]+" — "+json.dumps(check["detail"]))
    if host["pass"]:
        report.append("None in these short samples.")
    report.extend(["", *host["limitations"], ""])
    (output/"report.md").write_text("\n".join(report), encoding="utf-8")
    print(json.dumps(dict(samples_passed=host["samples_passed"], sample_count=len(host["samples"]),
                         calibration=str(output/"calibration.json"), accepted_calibration_receipt=False)), flush=True)
    return 0 if host["pass"] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run")
    run.add_argument("--output", required=True)
    run.add_argument("--profiles", default=str(Path(__file__).parent/"configs/profiles.json"))
    run.add_argument("--profile", action="append", choices=("ten_mbit_clean", "ten_mbit"))
    run.add_argument("--seed", action="append", type=int)
    run.add_argument("--seconds", type=float, default=5)
    run.add_argument("--paced-mbps", type=float, default=9)
    run.add_argument("--packet-bytes", type=int, default=1232)
    run.add_argument("--probe-hz", type=float, default=20)
    run.add_argument("--drain-s", type=float, default=.5)
    run.add_argument("--listen-port", type=int, default=43289)
    run.add_argument("--target-host", default="192.168.64.2")
    run.add_argument("--target-port", type=int, default=63389)
    finish = commands.add_parser("finalize")
    finish.add_argument("--output", required=True)
    finish.add_argument("--receiver-jsonl", required=True)
    finish.add_argument("--receiver-summary", required=True)
    args = parser.parse_args(argv)
    output = Path(args.output).resolve()
    if args.command == "finalize":
        return finalize(output, Path(args.receiver_jsonl), Path(args.receiver_summary))
    if not 0 < args.seconds <= 30 or not 0 < args.paced_mbps < 10 or not HEADER.size <= args.packet_bytes <= 1232 or not 0 < args.probe_hz <= 1000 or not .1 <= args.drain_s <= 5:
        parser.error("invalid bounded calibration settings")
    output.mkdir(exist_ok=True, parents=True)
    profiles = read_json(args.profiles)["profiles"]
    result = dict(version=1, environment=environment(), configuration=vars(args), samples=[],
                  sources={str(p.relative_to(Path(__file__).parent)): sha256(p) for p in
                           (Path(__file__), Path(__file__).parent/"live_relay.py",
                            Path(__file__).parent/"rdpbench/link.py", Path(__file__).parent/"windows/udp_calibration.py")},
                  scope="CRC UDP echo calibration, not RDP goodput", accepted_calibration_receipt=False)
    for profile_id in args.profile or ("ten_mbit_clean", "ten_mbit"):
        for seed in args.seed or (11, 29, 47):
            for mode in ("unloaded", "paced"):
                result["samples"].append(run_sample(args, output, profile_id, profiles[profile_id], seed, mode))
                write_json(output/"host-campaign.json", result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Owned, leased RDP proxy: seeded UDP impairment, reliable unshaped TCP.

This is a UDP-only network treatment. TCP control and fallback traffic bypass
the impairment model, so this must never be reported as a full-RDP 10 Mbit/s
profile. No packet payload, authentication data, or TLS secrets are recorded.

The adapter receives a final base64 JSON argument. Example command prefix:
  python live_relay.py adapter --state /tmp/rdp-network.json --output-dir /tmp/rdp
Requests: {op, run_id, profile_id, profile, seed, lease_s, trial_dir}. Only apply
requires profile and lease_s. Restore is scoped to the recorded run_id. The
separate child owns its sockets and expires on its own monotonic lease clock.
"""
from __future__ import annotations

import argparse
import base64
import collections
import errno
import fcntl
import heapq
import ipaddress
import json
import math
import os
from pathlib import Path
import queue
import select
import selectors
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid

from rdpbench.common import write_json
from rdpbench.link import Link, packet_metadata
from rdpbench.type1 import validate_profile


SCOPE = "UDP-only impairment; TCP control/fallback reliably forwarded, unshaped"
ENGINE = "live_rdp_udp_relay_v1"
TERMINAL = ("restored", "expired", "failed", "stopped")
ZERO_COUNTS = ("received_datagrams", "received_udp_bytes", "delivered_datagrams",
               "delivered_udp_bytes", "random_drops", "queue_drops", "blackout_drops",
               "duplicates", "reorder_events", "socket_send_errors", "late_over_1ms",
               "late_over_5ms", "max_lateness_us")
# In-process adapter callers retain and reap their own children. Separate CLI
# invocations use the control identity; they never signal an arbitrary PID.
OWNED_CHILDREN = {}


def encoded(value):
    return base64.b64encode(json.dumps(value, allow_nan=False).encode()).decode()


def decode(value):
    if len(value) > 262144:
        raise ValueError("oversized network adapter request")
    result = json.loads(base64.b64decode(value, validate=True))
    if not isinstance(result, dict) or result.get("op") not in ("apply", "status", "restore"):
        raise ValueError("expected apply, status or restore JSON object")
    if not isinstance(result.get("run_id"), str) or not result["run_id"] or len(result["run_id"]) > 128:
        raise ValueError("run_id is required and must be at most 128 characters")
    return result


def validate_endpoint(host, port):
    if ipaddress.ip_address(host).version != 4:
        raise ValueError("this relay models IPv4+UDP only")
    if isinstance(port, bool) or not isinstance(port, int) or not 0 <= port <= 65535:
        raise ValueError("invalid port")


class Ledger:
    """Bounded asynchronous metadata writer; overflow is visible, never hidden."""

    def __init__(self, path, capacity=32768):
        self.path = Path(path)
        self.stream = self.path.open("x", encoding="utf-8", buffering=1024*1024)
        self.queue = queue.Queue(capacity)
        self.lock = threading.Lock()
        self.count = 0
        self.overflow = 0
        self.error = None
        self.closed = False
        self.thread = threading.Thread(target=self.run, name="live-relay-ledger", daemon=True)
        self.thread.start()

    def add(self, event_type, **fields):
        try:
            self.queue.put_nowait(dict(type=event_type, **fields))
        except queue.Full:
            self.overflow += 1

    def run(self):
        try:
            while True:
                record = self.queue.get()
                if record is None:
                    break
                self.stream.write(json.dumps(record, separators=(",", ":"), allow_nan=False) + "\n")
                self.count += 1
            self.stream.flush()
        except BaseException as error:
            self.error = type(error).__name__ + ": " + str(error)
        finally:
            self.stream.close()

    def summary(self):
        return dict(path=str(self.path), format="jsonl_metadata_no_payload", count=self.count,
                    pending_records=self.queue.qsize(), capacity=self.queue.maxsize,
                    overflow=self.overflow, error=self.error, closed=self.closed,
                    valid=not self.overflow and self.error is None,
                    clock="monotonic_ns", scope="userspace UDP offer/drop/dispatch and TCP connection metadata")

    def close(self):
        if self.closed:
            return
        if self.thread.is_alive():
            # Bound teardown even if an output filesystem has stalled.
            try:
                self.queue.put(None, timeout=3)
            except queue.Full:
                self.error = self.error or "ledger writer did not drain"
            self.thread.join(5)
        if self.thread.is_alive():
            self.error = self.error or "ledger writer did not finish"
        self.closed = not self.thread.is_alive()


class LiveRelay:
    """One serial selector loop, shared seeded links, per-client UDP backend."""

    HEAP_CAPACITY = 65536
    UDP_FLOW_CAPACITY = 128
    TCP_CONNECTION_CAPACITY = 16
    TCP_BUFFER_CAPACITY = 1024*1024

    def __init__(self, config):
        self.config = config
        self.sel = selectors.DefaultSelector()
        self.sockets = set()
        self.heap = []
        self.serial = 0
        self.udp_flows = {}
        self.tcp = {}
        self.controls = {}
        self.links = [Link(config["profile"]["c2s"], config["seed"], "c2s"),
                      Link(config["profile"]["s2c"], config["seed"]+1000003, "s2c")]
        self.started_ns = time.monotonic_ns()
        self.deadline_ns = self.started_ns + int(config["lease_s"]*1e9)
        for link in self.links:
            link.start_ns = self.started_ns
        self.stop_reason = None
        self.error = None
        self.udp_errors = collections.Counter()
        self.tcp_counts = collections.Counter()
        self.tcp_direction = [collections.Counter(), collections.Counter()]
        self.lateness_histograms = [collections.Counter(), collections.Counter()]
        self.lateness_max_ns = [None, None]
        self.scheduler_counts = collections.Counter()
        self.teardown_started_ns = None
        self.ledger = None
        try:
            self.ledger = Ledger(config["ledger_path"])
            self.front = self.new_socket(socket.SOCK_DGRAM)
            self.front.bind((config["listen_host"], config["listen_port"]))
            self.port = self.front.getsockname()[1]
            self.sel.register(self.front, selectors.EVENT_READ, ("udp_front", None))
            self.listener = self.new_socket(socket.SOCK_STREAM)
            # Allow the next owned lease after this listener's connections enter
            # TIME_WAIT. An existing listener still prevents a second bind.
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.listener.bind((config["listen_host"], self.port))
            self.listener.listen(16)
            self.sel.register(self.listener, selectors.EVENT_READ, ("tcp_listener", None))
            self.control = self.new_socket(socket.SOCK_STREAM)
            self.control.bind(("127.0.0.1", 0))
            self.control.listen(8)
            self.sel.register(self.control, selectors.EVENT_READ, ("control_listener", None))
        except BaseException:
            self.close("failed")
            raise

    def new_socket(self, kind):
        value = socket.socket(socket.AF_INET, kind)
        self.sockets.add(value)
        value.setblocking(False)
        if kind == socket.SOCK_DGRAM:
            value.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
            value.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4*1024*1024)
        elif kind == socket.SOCK_STREAM:
            value.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return value

    def drop_socket(self, value):
        try:
            self.sel.unregister(value)
        except (KeyError, ValueError):
            pass
        self.sockets.discard(value)
        value.close()

    def update_socket(self, value, events, data):
        try:
            key = self.sel.get_key(value)
        except KeyError:
            if events:
                self.sel.register(value, events, data)
        else:
            if events:
                self.sel.modify(value, events, data)
            else:
                self.sel.unregister(value)

    def udp_flow(self, peer):
        if peer in self.udp_flows:
            return self.udp_flows[peer]
        if len(self.udp_flows) >= self.UDP_FLOW_CAPACITY:
            self.udp_errors["flow_capacity_drops"] += 1
            return None
        backend = self.new_socket(socket.SOCK_DGRAM)
        backend.connect((self.config["target_host"], self.config["target_port"]))
        flow = dict(peer=peer, backend=backend, id=len(self.udp_flows)+1)
        self.udp_flows[peer] = flow
        self.sel.register(backend, selectors.EVENT_READ, ("udp_back", flow))
        self.ledger.add("udp_flow", time_ns=time.monotonic_ns(), flow_id=flow["id"],
                        client=list(peer), proxy_backend=list(backend.getsockname()),
                        target=[self.config["target_host"], self.config["target_port"]])
        return flow

    def udp_offer(self, direction, flow, payload):
        now = time.monotonic_ns()
        link = self.links[direction]
        before = {key: link.count[key] for key in ("blackout_drops", "queue_drops", "random_drops")}
        scheduled = link.schedule(now, payload)
        metadata = packet_metadata(payload)
        self.ledger.add("offer", direction=link.direction, flow_id=flow["id"], packet_id=link.n,
                        offered_ns=now, scheduled_copies=len(scheduled), **metadata)
        if not scheduled:
            reason = next((key for key in before if link.count[key] > before[key]), "unknown_drop")
            self.ledger.add("drop", direction=link.direction, flow_id=flow["id"], packet_id=link.n,
                            offered_ns=now, reason=reason, **metadata)
        for due, data, unused_feedback in scheduled:
            if len(self.heap) >= self.HEAP_CAPACITY:
                raise RuntimeError("scheduled UDP capacity exceeded; invalid relay")
            self.serial += 1
            heapq.heappush(self.heap, (due, self.serial, direction, flow, data, link.n, now, metadata))

    def udp_read(self, front, flow=None):
        value = self.front if front else flow["backend"]
        for index in range(64):
            # A burst of arrivals must not hold expired deliveries until the
            # complete socket batch has been drained.
            if index % 8 == 0:
                self.udp_dispatch()
                if self.stop_reason is not None:
                    return
            try:
                if front:
                    payload, peer = value.recvfrom(65535)
                    current = self.udp_flow(peer)
                    if current is None:
                        self.udp_errors["flow_capacity_udp_bytes"] += len(payload)
                        continue
                else:
                    payload = value.recv(65535)
                    current = flow
            except BlockingIOError:
                return
            except OSError as error:
                self.udp_errors["socket_receive_errors"] += 1
                self.ledger.add("receive_error", time_ns=time.monotonic_ns(),
                                direction="c2s" if front else "s2c", error=str(error))
                return
            self.udp_offer(0 if front else 1, current, payload)

    def udp_dispatch(self):
        now = time.monotonic_ns()
        if self.stop_reason is not None:
            return
        if now >= self.deadline_ns:
            self.stop_reason = "expired"
            return
        dispatched = 0
        while self.heap and self.heap[0][0] <= now and dispatched < 256:
            if now >= self.deadline_ns:
                self.stop_reason = "expired"
                break
            due, copy_id, direction, flow, payload, packet_id, offered, metadata = heapq.heappop(self.heap)
            link = self.links[direction]
            try:
                sent = flow["backend"].send(payload) if direction == 0 else self.front.sendto(payload, flow["peer"])
                if sent != len(payload):
                    raise OSError("partial UDP datagram send")
                actual = time.monotonic_ns()
                link.delivered(due, actual, len(payload))
                lateness = max(0, actual-due)
                # Conservative quantile upper bounds with bounded geometry.
                # Exact nanosecond samples remain in the dispatch ledger.
                step = 10000 if lateness <= 10_000_000 else 1_000_000 if lateness <= 1_000_000_000 else 1_000_000_000
                bucket = ((lateness+step-1)//step)*step
                self.lateness_histograms[direction][bucket] += 1
                previous = self.lateness_max_ns[direction]
                self.lateness_max_ns[direction] = max(previous or 0, lateness)
                self.ledger.add("dispatch", direction=link.direction, flow_id=flow["id"],
                                packet_id=packet_id, copy_id=copy_id, offered_ns=offered,
                                due_ns=due, actual_dispatch_ns=actual,
                                lateness_ns=max(0, actual-due), **metadata)
            except OSError as error:
                link.count["socket_send_errors"] += 1
                self.udp_errors["socket_send_error_udp_bytes"] += len(payload)
                self.ledger.add("send_error", direction=link.direction, flow_id=flow["id"],
                                packet_id=packet_id, copy_id=copy_id, due_ns=due, error=str(error), **metadata)
            now = time.monotonic_ns()
            dispatched += 1

    def tcp_accept(self):
        for _ in range(16):
            try:
                front, peer = self.listener.accept()
            except BlockingIOError:
                return
            front.setblocking(False)
            front.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sockets.add(front)
            if len(self.tcp) >= self.TCP_CONNECTION_CAPACITY:
                self.tcp_counts["capacity_rejections"] += 1
                self.drop_socket(front)
                continue
            back = self.new_socket(socket.SOCK_STREAM)
            result = back.connect_ex((self.config["target_host"], self.config["target_port"]))
            self.tcp_counts["accepted_connections"] += 1
            ident = self.tcp_counts["accepted_connections"]
            connection = dict(id=ident, sockets=[front, back], out=[bytearray(), bytearray()],
                              eof=[False, False], shut=[False, False], connecting=result != 0,
                              received=[0, 0], forwarded=[0, 0], peer=peer)
            self.tcp[ident] = connection
            self.tcp_counts["connected_connections"] += result == 0
            self.ledger.add("tcp_accept", time_ns=time.monotonic_ns(), connection_id=ident,
                            client=list(peer), scope="reliable unshaped TCP")
            if result not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY):
                self.tcp_close(connection, os.strerror(result))
            else:
                self.tcp_interest(connection)

    def tcp_close(self, connection, error=None, error_side=None, error_operation=None, error_errno=None):
        if connection["id"] not in self.tcp:
            return
        pending = [len(buf) for buf in connection["out"]]
        now = time.monotonic_ns()
        owned_client_reset = (error is not None and error_errno == errno.ECONNRESET and error_side == 0
                              and not connection["connecting"] and not any(pending)
                              and self.teardown_started_ns is not None and now >= self.teardown_started_ns)
        self.tcp_counts["closed_connections"] += 1
        self.tcp_counts["socket_errors"] += error is not None
        self.tcp_counts["teardown_client_resets"] += owned_client_reset
        self.tcp_counts["active_socket_errors"] += error is not None and not owned_client_reset
        self.tcp_counts["discarded_pending_bytes"] += sum(pending)
        self.ledger.add("tcp_close", time_ns=now, connection_id=connection["id"],
                        received_bytes=connection["received"], forwarded_bytes=connection["forwarded"],
                        pending_bytes=pending, error=error, error_errno=error_errno,
                        error_side=("client", "target")[error_side] if error_side is not None else None,
                        error_operation=error_operation,
                        error_class="owned_client_reset" if owned_client_reset else "active_error" if error else None,
                        teardown_started_ns=self.teardown_started_ns)
        for value in connection["sockets"]:
            self.drop_socket(value)
        del self.tcp[connection["id"]]

    def tcp_interest(self, connection):
        if connection["id"] not in self.tcp:
            return
        for side, value in enumerate(connection["sockets"]):
            other = 1-side
            if connection["eof"][other] and not connection["out"][side] and not connection["shut"][side] and not connection["connecting"]:
                try:
                    value.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                connection["shut"][side] = True
            events = 0
            if side == 1 and connection["connecting"]:
                events |= selectors.EVENT_WRITE
            else:
                if not connection["eof"][side] and len(connection["out"][other]) < self.TCP_BUFFER_CAPACITY:
                    events |= selectors.EVENT_READ
                if connection["out"][side]:
                    events |= selectors.EVENT_WRITE
            self.update_socket(value, events, ("tcp", (connection, side)))
        if all(connection["eof"]) and not any(connection["out"]):
            self.tcp_close(connection)

    def tcp_event(self, connection, side, mask):
        if connection["id"] not in self.tcp:
            return
        value = connection["sockets"][side]
        operation = "connect" if side == 1 and connection["connecting"] else None
        try:
            if side == 1 and connection["connecting"]:
                error = value.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if error:
                    raise OSError(error, os.strerror(error))
                connection["connecting"] = False
                self.tcp_counts["connected_connections"] += 1
            if mask & selectors.EVENT_WRITE and connection["out"][side] and not connection["connecting"]:
                operation = "send"
                sent = value.send(connection["out"][side])
                del connection["out"][side][:sent]
                direction = 1-side
                connection["forwarded"][direction] += sent
                self.tcp_direction[direction]["forwarded_bytes"] += sent
            if mask & selectors.EVENT_READ:
                operation = "receive"
                other = 1-side
                space = self.TCP_BUFFER_CAPACITY-len(connection["out"][other])
                if space:
                    payload = value.recv(min(65536, space))
                    if payload:
                        connection["out"][other].extend(payload)
                        connection["received"][side] += len(payload)
                        self.tcp_direction[side]["received_bytes"] += len(payload)
                    else:
                        connection["eof"][side] = True
        except BlockingIOError:
            pass
        except OSError as error:
            self.tcp_close(connection, str(error), side, operation, error.errno)
            return
        self.tcp_interest(connection)

    def control_accept(self):
        for _ in range(8):
            try:
                value, peer = self.control.accept()
            except BlockingIOError:
                return
            self.sockets.add(value)
            value.setblocking(False)
            if len(self.controls) >= 16:
                self.drop_socket(value)
                continue
            record = dict(value=value, incoming=bytearray(), outgoing=bytearray(),
                          deadline=time.monotonic()+3, stop=False)
            self.controls[value] = record
            self.sel.register(value, selectors.EVENT_READ, ("control", record))

    def control_close(self, record):
        self.controls.pop(record["value"], None)
        self.drop_socket(record["value"])

    def control_event(self, record, mask):
        value = record["value"]
        try:
            if mask & selectors.EVENT_READ:
                payload = value.recv(65536)
                if not payload:
                    self.control_close(record)
                    return
                record["incoming"].extend(payload)
                if len(record["incoming"]) > 65536:
                    self.control_close(record)
                    return
                if b"\n" in record["incoming"]:
                    request = json.loads(record["incoming"].split(b"\n", 1)[0])
                    if not isinstance(request, dict):
                        raise ValueError("control request is not an object")
                    if request.get("token") != self.config["token"] or request.get("run_id") != self.config["run_id"]:
                        response = dict(verified=False, error="relay ownership mismatch")
                    elif request.get("op") not in ("status", "restore"):
                        response = dict(verified=False, error="unknown relay control operation")
                    else:
                        if request.get("begin_teardown") is True and self.teardown_started_ns is None:
                            self.teardown_started_ns = time.monotonic_ns()
                            self.ledger.add("owned_teardown_begin", time_ns=self.teardown_started_ns,
                                            run_id=self.config["run_id"])
                        response = self.snapshot()
                        record["stop"] = request["op"] == "restore"
                        if record["stop"]:
                            response["restore_requested"] = True
                    record["outgoing"] = bytearray(json.dumps(response, allow_nan=False).encode()+b"\n")
                    self.update_socket(value, selectors.EVENT_WRITE, ("control", record))
            if mask & selectors.EVENT_WRITE and record["outgoing"]:
                sent = value.send(record["outgoing"])
                del record["outgoing"][:sent]
                if not record["outgoing"]:
                    stop = record["stop"]
                    self.control_close(record)
                    if stop:
                        self.stop_reason = "restored"
        except (OSError, ValueError, TypeError):
            self.control_close(record)

    def direction_summary(self, direction):
        link = self.links[direction]
        result = {key: 0 for key in ZERO_COUNTS}
        result.update(link.count)
        histogram = sorted(self.lateness_histograms[direction].items())
        count = result["delivered_datagrams"]
        cumulative = 0
        quantiles = {"p50": None, "p95": None, "p99": None}
        ranks = {name: math.ceil(count*q) for name, q in (("p50", .5), ("p95", .95), ("p99", .99))}
        for upper_ns, population in histogram:
            cumulative += population
            for name, rank in ranks.items():
                if quantiles[name] is None and cumulative >= rank:
                    quantiles[name] = upper_ns/1e6
        result["lateness_ms"] = dict(count=count, **quantiles,
                                     max=None if self.lateness_max_ns[direction] is None else self.lateness_max_ns[direction]/1e6)
        result["lateness_quantile_method"] = "all-delivery upper bounds: 10us bins through 10ms, 1ms through 1s, 1s thereafter; exact max"
        result["lateness_sampled"] = False
        pending = [item for item in self.heap if item[2] == direction]
        result.update(pending_datagrams=len(pending), pending_udp_bytes=sum(len(item[4]) for item in pending),
                      profile=dict(link.p), seed=link.seed, counter_hits=result["received_datagrams"],
                      rate_layer="IPv4+UDP; UDP datagram bytes plus 28; Ethernet excluded")
        accounted = result["delivered_datagrams"]+len(pending)+sum(result[key] for key in
                    ("random_drops", "queue_drops", "blackout_drops", "socket_send_errors"))
        result["packet_accounting"] = dict(received=result["received_datagrams"],
                                           created_copies=result["duplicates"], accounted=accounted,
                                           balanced=result["received_datagrams"]+result["duplicates"] == accounted)
        return result

    def snapshot(self, state="active"):
        directions = {name: self.direction_summary(i) for i, name in enumerate(("c2s", "s2c"))}
        elapsed_s = max(0, (time.monotonic_ns()-self.started_ns)/1e9)
        return dict(version=1, engine=ENGINE, verified=True, active=state == "active", state=state,
                    valid=self.error is None and (self.ledger is None or self.ledger.summary()["valid"]),
                    scope=SCOPE, full_rdp_profile=False, tcp_impaired=False, udp_impaired=True,
                    run_id=self.config["run_id"], profile_id=self.config["profile_id"],
                    profile=self.config["profile"], seed=self.config["seed"],
                    lease_id=self.config["lease_id"], pid=os.getpid(), lease_s=self.config["lease_s"],
                    started_ns=self.started_ns, deadline_ns=self.deadline_ns,
                    teardown_started_ns=self.teardown_started_ns,
                    remaining_lease_s=max(0, (self.deadline_ns-time.monotonic_ns())/1e9),
                    elapsed_s=elapsed_s,
                    listen=[self.config["listen_host"], getattr(self, "port", self.config["listen_port"])],
                    target=[self.config["target_host"], self.config["target_port"]],
                    control=["127.0.0.1", self.control.getsockname()[1]] if hasattr(self, "control") and self.control.fileno() >= 0 else None,
                    capturepath=self.config["ledger_path"], capture_format="jsonl_metadata_no_payload",
                    ledger=self.ledger.summary() if self.ledger else None,
                    directions=directions, c2s=directions["c2s"], s2c=directions["s2c"],
                    udp_counter_hits_both_directions=all(value["counter_hits"] > 0 for value in directions.values()),
                    udp_flow_count=len(self.udp_flows), udp_errors=dict(self.udp_errors),
                    tcp_connections=dict({name: 0 for name in ("socket_errors", "active_socket_errors", "teardown_client_resets", "discarded_pending_bytes")},
                                         **dict(self.tcp_counts, active_connections=len(self.tcp))),
                    tcp_error_policy="total socket_errors retained; only established client ECONNRESET with zero pending bytes after explicit owned teardown is exempt from active_socket_errors",
                    scheduler=dict(self.scheduler_counts),
                    tcp={"scope": "proxy TCP bytes, reliable/unshaped; not verified application goodput",
                         **{name: dict(received_bytes=self.tcp_direction[i]["received_bytes"],
                                       forwarded_bytes=self.tcp_direction[i]["forwarded_bytes"],
                                       pending_bytes=sum(len(c["out"][1-i]) for c in self.tcp.values()))
                            for i, name in enumerate(("c2s", "s2c"))}},
                    kernel_drop_counter=None, error=self.error)

    def run(self, ready_fd=None):
        try:
            ready = self.snapshot()
            write_json(self.config["status_path"], ready)
            if ready_fd is not None:
                os.write(ready_fd, json.dumps(ready, allow_nan=False).encode()+b"\n")
                os.close(ready_fd)
                ready_fd = None
            while self.stop_reason is None:
                now = time.monotonic_ns()
                if now >= self.deadline_ns:
                    self.stop_reason = "expired"
                    break
                self.udp_dispatch()
                timeout = min(.01, (self.deadline_ns-now)/1e9)
                if self.heap:
                    timeout = min(timeout, max(0, (self.heap[0][0]-now)/1e9))
                next_due = self.heap[0][0] if self.heap else None
                select_begin = time.monotonic_ns()
                ready_events = self.sel.select(timeout)
                select_end = time.monotonic_ns()
                overshoot = max(0, select_end-select_begin-int(timeout*1e9))
                self.scheduler_counts["iterations"] += 1
                self.scheduler_counts["max_select_overshoot_us"] = max(self.scheduler_counts["max_select_overshoot_us"], overshoot//1000)
                if overshoot > 1_000_000:
                    self.scheduler_counts["select_over_1ms"] += 1
                    self.ledger.add("scheduler_gap", kind="select_interval", begin_ns=select_begin,
                                    end_ns=select_end, requested_timeout_ns=int(timeout*1e9),
                                    overshoot_ns=overshoot, next_due_ns=next_due,
                                    ready_event_count=len(ready_events),
                                    scope="selector interval includes unmeasured host scheduling")
                self.udp_dispatch()
                for key, mask in ready_events:
                    if self.stop_reason is not None:
                        break
                    kind, value = key.data
                    handler_begin = time.monotonic_ns()
                    if kind == "udp_front": self.udp_read(True)
                    elif kind == "udp_back": self.udp_read(False, value)
                    elif kind == "tcp_listener": self.tcp_accept()
                    elif kind == "tcp": self.tcp_event(*value, mask)
                    elif kind == "control_listener": self.control_accept()
                    elif kind == "control": self.control_event(value, mask)
                    handler_end = time.monotonic_ns()
                    duration = handler_end-handler_begin
                    self.scheduler_counts["max_io_handler_us"] = max(self.scheduler_counts["max_io_handler_us"], duration//1000)
                    if duration > 1_000_000:
                        self.scheduler_counts["io_handler_over_1ms"] += 1
                        self.ledger.add("scheduler_gap", kind="io_handler", handler=kind,
                                        begin_ns=handler_begin, end_ns=handler_end, duration_ns=duration,
                                        scope="handler interval includes unmeasured host scheduling")
                    self.udp_dispatch()
                self.udp_dispatch()
                for record in list(self.controls.values()):
                    if record["deadline"] <= time.monotonic():
                        self.control_close(record)
        except BaseException as error:
            self.error = type(error).__name__+": "+str(error)
            self.stop_reason = "failed"
        finally:
            if ready_fd is not None:
                os.close(ready_fd)
            result = self.close(self.stop_reason or "stopped")
            write_json(self.config["status_path"], result)
        return result

    def close(self, state="stopped"):
        for connection in list(self.tcp.values()):
            self.tcp_close(connection, "lease teardown" if any(connection["out"]) else None)
        for value in list(self.sockets):
            self.drop_socket(value)
        self.sel.close()
        if self.ledger:
            self.ledger.add("lease_end", time_ns=time.monotonic_ns(), state=state,
                            pending_datagrams=len(self.heap), pending_udp_bytes=sum(len(item[4]) for item in self.heap))
            self.ledger.close()
        result = self.snapshot(state)
        result["sockets_closed"] = True
        result["control"] = None
        result["valid"] = self.error is None and (self.ledger is None or self.ledger.summary()["valid"])
        return result


def rpc(reference, op, begin_teardown=False):
    with socket.create_connection(tuple(reference["control"]), timeout=3) as value:
        value.sendall(json.dumps(dict(op=op, token=reference["token"], run_id=reference["run_id"],
                                      begin_teardown=begin_teardown)).encode()+b"\n")
        pending = bytearray()
        while b"\n" not in pending:
            chunk = value.recv(65536)
            if not chunk:
                raise RuntimeError("relay control closed without a response")
            pending.extend(chunk)
            if len(pending) > 4*1024*1024:
                raise RuntimeError("oversized relay control response")
        return json.loads(pending.split(b"\n", 1)[0])


def terminal_status(reference):
    path = Path(reference["status_path"])
    if not path.is_file():
        return None
    result = json.loads(path.read_text())
    if result.get("lease_id") == reference["lease_id"] and result.get("run_id") == reference["run_id"] and result.get("state") in TERMINAL and result.get("sockets_closed"):
        process = OWNED_CHILDREN.get(reference["lease_id"])
        if process is not None:
            try:
                process.wait(timeout=1)
                del OWNED_CHILDREN[reference["lease_id"]]
            except subprocess.TimeoutExpired:
                pass
        return result
    return None


def live_status(reference, begin_teardown=False):
    try:
        result = rpc(reference, "status", begin_teardown)
        if result.get("verified") and result.get("lease_id") == reference["lease_id"]:
            return result
        raise RuntimeError("relay identity verification failed")
    except (OSError, RuntimeError, KeyError):
        result = terminal_status(reference)
        if result is not None:
            return result
        raise RuntimeError("recorded relay has no verified live control or terminal teardown; refusing to overwrite")


def apply(args, request, state):
    if state.exists():
        reference = json.loads(state.read_text())
        if live_status(reference).get("active"):
            raise RuntimeError("a live relay already owns this state; restore its recorded run first")
    profile = request.get("profile")
    validate_profile(profile)
    if any(profile[name].get("overhead_bytes", 28) != 28 for name in ("c2s", "s2c")):
        raise ValueError("live IPv4 UDP profiles require exactly 28 overhead bytes")
    seed = request.get("seed", 11)
    if isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed < 2**63:
        raise ValueError("seed must be an integer in [0, 2**63)")
    lease = request.get("lease_s")
    if isinstance(lease, bool) or not isinstance(lease, (int, float)) or not math.isfinite(lease) or not .25 <= lease <= 3600:
        raise ValueError("lease_s must be between 0.25 and 3600 seconds")
    if not isinstance(request.get("profile_id"), str) or not request["profile_id"]:
        raise ValueError("profile_id is required")
    validate_endpoint(args.listen_host, args.listen_port)
    validate_endpoint(args.target_host, args.target_port)
    if args.target_port == 0:
        raise ValueError("target port must not be zero")
    lease_id = uuid.uuid4().hex
    base = Path(request["trial_dir"])/"live-network" if request.get("trial_dir") else Path(args.output_dir)
    base = base.expanduser().resolve()
    base.mkdir(parents=True, exist_ok=True)
    folder = base/("lease-"+lease_id)
    folder.mkdir(mode=0o700, exist_ok=False)
    config = dict(version=1, run_id=request["run_id"], profile_id=request["profile_id"], profile=profile,
                  seed=seed, lease_s=lease, lease_id=lease_id, token=uuid.uuid4().hex,
                  listen_host=args.listen_host, listen_port=args.listen_port,
                  target_host=args.target_host, target_port=args.target_port,
                  ledger_path=str(folder/"relay.jsonl"), status_path=str(folder/"status.json"))
    config_path = folder/"config.json"
    with config_path.open("x", encoding="utf-8") as stream:
        os.chmod(config_path, 0o600)
        json.dump(config, stream, indent=2, allow_nan=False)
    read_fd, write_fd = os.pipe()
    log = (folder/"child.stderr").open("x", encoding="utf-8")
    process = None
    try:
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "_serve",
                                    "--config", str(config_path), "--ready-fd", str(write_fd)],
                                   pass_fds=(write_fd,), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=log, start_new_session=True)
        os.close(write_fd)
        write_fd = None
        pending = bytearray()
        deadline = time.monotonic()+8
        while b"\n" not in pending:
            ready, unused, unused2 = select.select([read_fd], [], [], max(0, deadline-time.monotonic()))
            if not ready:
                raise RuntimeError("relay bind/readiness timed out")
            chunk = os.read(read_fd, 65536)
            if not chunk:
                raise RuntimeError("relay failed before bind/readiness; inspect "+str(folder/"child.stderr"))
            pending.extend(chunk)
        receipt = json.loads(pending.split(b"\n", 1)[0])
        reference = {key: config[key] for key in ("run_id", "profile_id", "lease_id", "token", "status_path")}
        reference.update(version=1, engine=ENGINE, control=receipt["control"], pid=process.pid, config_path=str(config_path))
        # A real IPC round-trip confirms the bound child, not a stale state file.
        receipt = rpc(reference, "status")
        if not receipt.get("verified") or not receipt.get("active") or receipt.get("lease_id") != lease_id:
            raise RuntimeError("relay was not live after bind readiness")
        write_json(state, reference)
        os.chmod(state, 0o600)
        OWNED_CHILDREN[lease_id] = process
        return receipt
    except BaseException:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        raise
    finally:
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)
        log.close()


def adapter(args):
    request = decode(args.request)
    if "begin_teardown" in request and (request["op"] != "status" or not isinstance(request["begin_teardown"], bool)):
        raise ValueError("begin_teardown must be a boolean on an owned status request")
    state = Path(args.state).expanduser().resolve()
    state.parent.mkdir(parents=True, exist_ok=True)
    with Path(str(state)+".lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("another operation owns this relay state")
        if request["op"] == "apply":
            return apply(args, request, state)
        if not state.is_file():
            if request["op"] == "restore":
                return dict(version=1, engine=ENGINE, verified=True, active=False, state="absent",
                            run_id=request["run_id"], profile_id=request.get("profile_id"), scope=SCOPE,
                            sockets_closed=True, full_rdp_profile=False, tcp_impaired=False)
            raise RuntimeError("no recorded relay state")
        reference = json.loads(state.read_text())
        if request["run_id"] != reference["run_id"]:
            raise RuntimeError("network operation run_id does not own the recorded relay")
        if request.get("profile_id") and request["profile_id"] != reference["profile_id"]:
            raise RuntimeError("network operation profile_id mismatch")
        receipt = live_status(reference, request.get("begin_teardown", False))
        if request["op"] == "restore" and receipt.get("active"):
            rpc(reference, "restore")
            deadline = time.monotonic()+10
            while time.monotonic() < deadline:
                receipt = terminal_status(reference)
                if receipt is not None:
                    return receipt
                time.sleep(.02)
            raise RuntimeError("relay restore did not verify closed sockets")
        return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("adapter", help="apply/status/restore the owned UDP-only relay")
    command.add_argument("--state", required=True)
    command.add_argument("--output-dir", required=True)
    command.add_argument("--listen-host", default="127.0.0.1")
    command.add_argument("--listen-port", type=int, default=43389)
    command.add_argument("--target-host", default="192.168.64.2")
    command.add_argument("--target-port", type=int, default=3389)
    command.add_argument("request", help="base64-encoded JSON, final adapter argument")
    server = commands.add_parser("_serve", help=argparse.SUPPRESS)
    server.add_argument("--config", required=True)
    server.add_argument("--ready-fd", required=True, type=int)
    args = parser.parse_args(argv)
    try:
        if args.command == "adapter":
            result = adapter(args)
            print(json.dumps(result, allow_nan=False))
            return 0
        config = json.loads(Path(args.config).read_text())
        relay = LiveRelay(config)
        def stop(signum, frame):
            relay.stop_reason = "stopped"
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        result = relay.run(args.ready_fd)
        return 1 if result["state"] == "failed" else 0
    except BaseException as error:
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        failure = dict(verified=False, error=type(error).__name__+": "+str(error))
        if args.command == "_serve":
            print(json.dumps(failure), file=sys.stderr)
            config = json.loads(Path(args.config).read_text())
            failure.update(state="failed", active=False, sockets_closed=True,
                           run_id=config["run_id"], lease_id=config["lease_id"], profile_id=config["profile_id"])
            write_json(config["status_path"], failure)
        print(json.dumps(failure), file=sys.stdout)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

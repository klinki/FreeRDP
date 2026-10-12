"""Live Windows RDP campaign with independently observed, numbered input probes.

Transport rates, render durations and end-to-end interaction are distinct
metrics. Missing responses remain failures/censored evidence, never zero latency.
"""
from __future__ import annotations

import base64
import errno
import json
import math
import os
import random
import re
import shutil
import subprocess
import threading
import time
import uuid
from pathlib import Path

from .bridge import Client, child_environment, fingerprint, managed_argument, sensitive_env_name
from .common import Journal, audit, canonical, digest, environment, read_json, rows, seal, sha256, timing_summary, write_json

ROOT = Path(__file__).resolve().parents[1]
SCENES = {"text-scroll", "web-scroll", "canvas-motion", "typing-visual", "static-idle", "video"}


def ps_quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def ssh_options(lab):
    result = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10"]
    if lab.get("ssh_control_path"):
        result += ["-o", "ControlPath=" + lab["ssh_control_path"], "-o", "ControlMaster=no"]
    return result


def ps(lab, script, timeout=20):
    script = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';" + script
    encoded = base64.b64encode(script.encode("utf-16le")).decode()
    return subprocess.check_output(["ssh", *ssh_options(lab), lab["ssh_target"],
                                    "powershell.exe", "-NoProfile", "-EncodedCommand", encoded],
                                   text=True, timeout=timeout, env=child_environment(lab)).lstrip("\ufeff").strip()


def same_identity(before, after):
    keys = ("agent_uuid", "pid", "windows_session_id")
    return all(before.get(key) is not None and before.get(key) == after.get(key) for key in keys)


def agent(lab, op, run_id, trigger=False, timeout=40, **data):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", run_id):
        raise ValueError("unsafe Windows run ID")
    request = {"request_id": uuid.uuid4().hex, "run_id": run_id, "op": op, **data}
    root = lab["windows_root"].rstrip("\\/")
    payload = base64.b64encode(canonical(request)).decode()
    path = root + "\\command.json"
    script = (f"$b=[Convert]::FromBase64String('{payload}');"
              f"[IO.File]::WriteAllBytes({ps_quote(path + '.tmp')},$b);"
              f"Move-Item -Force -LiteralPath {ps_quote(path + '.tmp')} -Destination {ps_quote(path)};")
    if trigger:
        script += f"Start-ScheduledTask -TaskName {ps_quote(lab['windows_task'])};"
    ps(lab, script)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = ps(lab, f"if(Test-Path -LiteralPath {ps_quote(root + '\\status.json')})"
                  f"{{[IO.File]::ReadAllText({ps_quote(root + '\\status.json')})}}", timeout=10)
        try:
            state = json.loads(text) if text else {}
        except json.JSONDecodeError:
            state = {}
        if state.get("request_id") == request["request_id"]:
            if state.get("error"):
                raise RuntimeError("Windows agent: " + str(state["error"]))
            if state.get("run_id") != run_id:
                raise RuntimeError("Windows agent acknowledged a different run")
            return state
        time.sleep(.2)
    raise TimeoutError(f"Windows agent did not acknowledge {op}")


def collect_windows(lab, run_id, folder):
    remote = lab["windows_root"].replace("\\", "/").rstrip("/") + "/runs/" + run_id
    subprocess.run(["scp", *ssh_options(lab), "-r", lab["ssh_target"] + ":" + remote,
                    str(Path(folder) / "windows")], check=True, timeout=60,
                   stdout=subprocess.DEVNULL, env=child_environment(lab))
    if not (Path(folder) / "windows" / "agent.jsonl").is_file():
        raise RuntimeError("Windows agent log was not collected")


def expanded_command(command, folder, run_id, client_pid=None):
    return [str(value).replace("{trial_dir}", str(Path(folder).resolve())).replace("{run_id}", run_id)
            .replace("{client_pid}", str(client_pid) if client_pid is not None else "{client_pid}")
            for value in command]


def configured_display_errors(lab, payload):
    """Pin active physical display UUIDs and the calibrated observer ID."""
    errors, selected = [], []
    selections = lab.get("display_selections", [])
    if not isinstance(selections, list):
        return ["configured display selections are invalid"]
    for selection in selections:
        value = selection.get("uuid") if isinstance(selection, dict) else None
        if not isinstance(value, str) or not value:
            errors.append("configured display UUID is missing or invalid")
        else:
            selected.append(value.lower())
    observer = lab.get("observer", {})
    command = observer.get("command", [])
    requested_ids = []
    if isinstance(command, list):
        for index, value in enumerate(command):
            if value == "--display":
                if index + 1 >= len(command):
                    errors.append("observer --display has no configured ID")
                    continue
                try:
                    item = int(str(command[index + 1]))
                except ValueError:
                    item = 0
                if item <= 0:
                    errors.append("observer configured display ID is invalid")
                else:
                    requested_ids.append(item)
    if len(set(selected)) != len(selected) or len(requested_ids) > 1:
        errors.append("configured display UUIDs or observer display IDs are ambiguous")
    if not selected and not requested_ids:
        return errors
    inventory = payload.get("active_displays")
    if payload.get("display_inventory_valid") is not True or not isinstance(inventory, list):
        return errors + ["active display inventory is missing, invalid or unknown"]
    by_id, by_uuid = {}, {}
    for entry in inventory:
        if not isinstance(entry, dict):
            errors.append("active display inventory contains an invalid entry")
            continue
        display_id = integer_number(entry.get("display_id"))
        uuid_value = entry.get("uuid")
        if display_id is None or display_id == 0 or not isinstance(uuid_value, str) or not uuid_value:
            errors.append("active display ID/UUID identity is missing or invalid")
            continue
        uuid_value = uuid_value.lower()
        if display_id in by_id or uuid_value in by_uuid:
            errors.append("active physical display identities are ambiguous")
        by_id[display_id], by_uuid[uuid_value] = uuid_value, display_id
    for selected_uuid in selected:
        if selected_uuid not in by_uuid:
            errors.append("required physical display UUID is not active: " + selected_uuid)
    for display_id in requested_ids:
        if display_id not in by_id:
            errors.append("calibrated observer display ID is not active: " + str(display_id))
        elif selected and by_id[display_id] not in selected:
            errors.append("observer display ID does not belong to a selected physical display UUID")
    return list(dict.fromkeys(errors))


def environment_preflight(lab, folder, run_id):
    """Read desktop availability before creating or launching any live trial."""
    command = lab.get("observer", {}).get("environment_command")
    if command is None:
        observer_command = lab.get("observer", {}).get("command", [])
        if lab.get("display_selections") or (isinstance(observer_command, list) and "--display" in observer_command):
            return {"status": "unavailable", "available": False, "required": True,
                    "reasons": ["configured physical displays require an active-display environment command"]}
        return {"status": "not_configured", "available": None, "required": False,
                "reasons": ["optional desktop environment command is not configured"]}
    result = {"status": "unavailable", "available": False, "required": True,
              "checked_ns": time.monotonic_ns(), "observed": {}, "reasons": []}
    if not isinstance(command, list) or not command or any(not isinstance(value, str) for value in command):
        result["reasons"].append("desktop environment command is empty or invalid")
        return result
    try:
        completed = subprocess.run(expanded_command(command, folder, run_id),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, timeout=15,
                                   env=child_environment(lab), check=False)
        result["returncode"] = completed.returncode
        lines = [line for line in completed.stdout.splitlines() if line.strip()]
        if len(lines) != 1:
            raise ValueError("desktop helper did not return exactly one JSON record")
        payload = json.loads(lines[0])
        if not isinstance(payload, dict):
            raise ValueError("desktop helper result is not an object")
        names = ("screen_locked", "console_session", "login_complete", "login_window_foreground",
                 "capture_allowed", "accessibility_allowed", "available", "display_inventory_valid", "active_displays")
        result["observed"] = {name: payload.get(name) for name in names}
        result["reasons"].extend(configured_display_errors(lab, payload))
        if completed.returncode != 0:
            result["reasons"].append("desktop environment helper exited nonzero")
        # Private macOS lock dictionary keys can be absent while unlocked.
        # The helper's computed availability is the public gate contract.
        if payload.get("available") is not True:
            result["reasons"].append("desktop availability is false, missing or unknown")
        if not result["reasons"]:
            result.update(status="available", available=True)
    except Exception as error:
        result["reasons"].append("desktop environment evidence failed: " + type(error).__name__)
    return result


def record_environment_preflight(lab, base, plan):
    record = environment_preflight(lab, base, plan["campaign_id"])
    plan.setdefault("environment_preflight_history", []).append(record)
    plan["environment_preflight"] = record
    write_json(Path(base) / "preflight-environment.json", {
        "version": 1, "campaign_id": plan["campaign_id"],
        "latest": record, "checks": plan["environment_preflight_history"]})
    write_json(Path(base) / "campaign.json", plan)
    write_json(Path(base) / "live-results.json", aggregate(base, plan, lab))
    return not record["required"] or record["available"] is True


def network(lab, op, run_id, timeline, **data):
    command = lab.get("network", {}).get(op + "_command")
    if not command:
        raise RuntimeError("missing verified live network adapter for " + op)
    request = {"op": op, "run_id": run_id, **data}
    encoded = base64.b64encode(canonical(request)).decode()
    receipt = json.loads(subprocess.check_output([*command, encoded], text=True, timeout=30,
                                               env=child_environment(lab)))
    timeline.emit("network_receipt", operation=op, request=request, receipt=receipt)
    if receipt.get("verified") is not True:
        raise RuntimeError("live network adapter did not verify " + op)
    if receipt.get("run_id") != run_id:
        raise RuntimeError("live network receipt belongs to another run")
    if "profile_id" in data and receipt.get("profile_id") != data["profile_id"]:
        raise RuntimeError("live network receipt describes another profile")
    return receipt


def integer_number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0 or int(value) != value:
        return None
    return int(value)


def receipt_errors(receipt, operation, profile, seed, timing_budget_ms=1.5, teardown_evidence=None):
    """Verified ownership alone is insufficient to admit a network treatment."""
    errors = []
    if receipt.get("valid") is not True:
        errors.append("relay valid flag is missing or false")
    expected_active = operation != "restore"
    if receipt.get("active") is not expected_active:
        errors.append("relay active state is wrong for " + operation)
    if receipt.get("profile") != profile or receipt.get("seed") != seed:
        errors.append("relay profile or seed differs from the requested treatment")
    if receipt.get("error"):
        errors.append("relay reported an error")
    ledger = receipt.get("ledger") or {}
    if ledger.get("valid") is not True or integer_number(ledger.get("overflow")) != 0 or ledger.get("error"):
        errors.append("relay ledger is missing, invalid, overflowed or errored")
    if operation == "restore" and (receipt.get("sockets_closed") is not True or ledger.get("closed") is not True):
        errors.append("relay sockets or ledger were not confirmed closed")
    if operation == "restore" and integer_number(ledger.get("pending_records")) != 0:
        errors.append("closed relay ledger still has pending or unknown records")
    if not isinstance(receipt.get("udp_errors"), dict):
        errors.append("relay UDP error counters are unknown")
    for name, value in (receipt.get("udp_errors") or {}).items():
        if integer_number(value) != 0:
            errors.append("relay UDP error counter " + name)
    tcp = receipt.get("tcp_connections") or {}
    if "active_socket_errors" in tcp or "teardown_client_resets" in tcp:
        total, active, resets = [integer_number(tcp.get(name)) for name in
                                 ("socket_errors", "active_socket_errors", "teardown_client_resets")]
        if None in (total, active, resets) or total != active + resets:
            errors.append("relay TCP error split is unknown or does not reconcile with total")
        if active is None or active:
            errors.append("relay TCP active socket errors")
        if resets:
            marker = integer_number(receipt.get("teardown_started_ns"))
            proof = teardown_evidence or {}
            if marker is None or marker != proof.get("marker_ns") or integer_number(proof.get("client_cancel_ns")) is None or proof["client_cancel_ns"] < marker:
                errors.append("relay TCP reset exemption lacks explicit owned client-cancel evidence")
    elif integer_number(tcp.get("socket_errors", 0)) != 0:
        # Older receipts have no teardown proof. Their errors remain errors.
        errors.append("relay TCP error counter socket_errors")
    for name in ("capacity_rejections", "discarded_pending_bytes"):
        if integer_number(tcp.get(name, 0)) != 0:
            errors.append("relay TCP error counter " + name)
    for direction in ("c2s", "s2c"):
        values = receipt.get(direction) or {}
        account = values.get("packet_accounting") or {}
        received, copies, accounted = [integer_number(account.get(name)) for name in ("received", "created_copies", "accounted")]
        if account.get("balanced") is not True or None in (received, copies, accounted) or received + copies != accounted:
            errors.append(direction + " packet accounting is unknown or unbalanced")
        if values.get("profile") != profile.get(direction) or values.get("seed") != seed + (1000003 if direction == "s2c" else 0):
            errors.append(direction + " effective profile or seed mismatch")
        pending = integer_number(values.get("pending_datagrams"))
        pending_bytes = integer_number(values.get("pending_udp_bytes"))
        if pending is None or pending_bytes is None or (pending == 0 and pending_bytes != 0):
            errors.append(direction + " pending datagrams/bytes are not explicitly accounted")
        if integer_number(values.get("socket_send_errors")) != 0:
            errors.append(direction + " socket-send accounting is missing or failed")
        hits = integer_number(values.get("counter_hits"))
        if operation != "apply" and (hits is None or hits == 0):
            errors.append(direction + " has no verified UDP counter hits")
        latency = (values.get("lateness_ms") or {}).get("p99")
        if hits and (not isinstance(latency, (int, float)) or isinstance(latency, bool) or not math.isfinite(latency)):
            errors.append(direction + " UDP dispatch timing is unknown")
        elif isinstance(latency, (int, float)) and latency > timing_budget_ms:
            errors.append(f"{direction} relay p99 lateness exceeds {timing_budget_ms:g} ms")
    return errors


def begin_owned_teardown(lab, run_id, profile_id, profile, seed, timeline, receipts, budget_ms):
    requested = time.monotonic_ns()
    receipt = network(lab, "status", run_id, timeline, profile_id=profile_id, begin_teardown=True)
    received = time.monotonic_ns()
    receipts.append(receipt)
    errors = receipt_errors(receipt, "status", profile, seed, budget_ms)
    marker = integer_number(receipt.get("teardown_started_ns"))
    if marker is None or not requested <= marker <= received:
        errors.append("relay did not confirm a fresh explicit owned teardown marker")
        return {}, errors
    return {"marker_ns": marker, "requested_ns": requested, "acknowledged_ns": received}, errors


def udp_activity_delta(before, after):
    result, errors = {}, []
    for direction in ("c2s", "s2c"):
        left = integer_number((before.get(direction) or {}).get("counter_hits"))
        right = integer_number((after.get(direction) or {}).get("counter_hits"))
        delta = right - left if left is not None and right is not None else None
        result[direction] = delta
        if delta is None or delta <= 0:
            errors.append(direction + " has no positive UDP packet delta during the measured phase")
    return result, errors


def ledger_evidence(receipt, interval, folder, timing_budget_ms=1.5, teardown_evidence=None):
    """Audit the closed ledger and select exact measured-phase dispatch timing."""
    path = Path(receipt.get("capturepath", "")).resolve()
    if not path.is_file() or not path.is_relative_to(Path(folder).resolve()):
        return {}, ["closed relay ledger is unavailable inside this trial"]
    seen, offer_records, remaining, dispatched, drops, send_errors, pending, errors = {}, {}, {}, set(), {"c2s": 0, "s2c": 0}, {"c2s": 0, "s2c": 0}, {"c2s": 0, "s2c": 0}, []
    selected = {direction: {"offers": 0, "dispatches": 0, "data_offers": 0, "data_dispatches": 0, "lateness": []}
                for direction in ("c2s", "s2c")}
    record_count = 0
    teardown_markers, tcp_error_rows = [], []
    for entry in rows(path):
        record_count += 1
        kind, direction = entry.get("type"), entry.get("direction")
        if kind == "owned_teardown_begin":
            teardown_markers.append(entry)
        if kind == "tcp_close" and entry.get("error"):
            tcp_error_rows.append(entry)
        if direction not in selected:
            continue
        key = (direction, entry.get("packet_id"))
        if kind == "offer":
            copies = integer_number(entry.get("scheduled_copies"))
            if key in seen or copies is None or integer_number(entry.get("packet_id")) is None:
                errors.append("duplicate or invalid relay offer")
                continue
            seen[key] = copies
            offer_records[key] = entry
            remaining[key] = copies
            pending[direction] += copies
            if interval["begin_ns"] <= entry.get("offered_ns", -1) < interval["end_ns"]:
                selected[direction]["offers"] += 1
                selected[direction]["data_offers"] += str(entry.get("kind", "")).startswith("data_")
        elif kind == "drop":
            drops[direction] += 1
            if key not in seen or seen[key] != 0:
                errors.append("unmatched relay drop")
        elif kind in ("dispatch", "send_error"):
            identity = (direction, entry.get("copy_id"))
            if key not in seen or remaining.get(key, 0) <= 0 or identity in dispatched:
                errors.append("unmatched or duplicate relay dispatch")
            remaining[key] = remaining.get(key, 0) - 1
            dispatched.add(identity)
            pending[direction] -= 1
            if kind == "send_error":
                send_errors[direction] += 1
                continue
            actual, due = entry.get("actual_dispatch_ns"), entry.get("due_ns")
            if integer_number(actual) is None or integer_number(due) is None or actual < due:
                errors.append("invalid relay dispatch timestamps")
                continue
            if due < interval["end_ns"] and actual >= interval["begin_ns"]:
                selected[direction]["lateness"].append((actual - due) / 1e6)
                boundary = selected[direction]
                boundary["due_before_measurement"] = boundary.get("due_before_measurement", 0) + (due < interval["begin_ns"])
                boundary["dispatched_after_measurement"] = boundary.get("dispatched_after_measurement", 0) + (actual >= interval["end_ns"])
            if interval["begin_ns"] <= actual < interval["end_ns"]:
                selected[direction]["dispatches"] += 1
                selected[direction]["data_dispatches"] += str(entry.get("kind", "")).startswith("data_")
        if kind in ("drop", "dispatch", "send_error") and key in offer_records:
            metadata = {name: value for name, value in offer_records[key].items()
                        if name not in ("type", "direction", "packet_id", "scheduled_copies")}
            if any(entry.get(name) != value for name, value in metadata.items()):
                errors.append("relay drop/dispatch metadata does not match its offer")
    if record_count != integer_number((receipt.get("ledger") or {}).get("count")):
        errors.append("relay ledger record count disagrees with final receipt")
    tcp = receipt.get("tcp_connections") or {}
    if "active_socket_errors" in tcp or "teardown_client_resets" in tcp:
        marker = integer_number(receipt.get("teardown_started_ns"))
        proof = teardown_evidence or {}
        exempt = []
        for entry in tcp_error_rows:
            if entry.get("error_class") != "owned_client_reset":
                continue
            tcp_pending = entry.get("pending_bytes")
            forwarded, received = entry.get("forwarded_bytes"), entry.get("received_bytes")
            valid = (marker is not None and marker == proof.get("marker_ns")
                     and integer_number(proof.get("client_cancel_ns")) is not None
                     and integer_number(entry.get("time_ns")) is not None
                     and entry["time_ns"] >= proof["client_cancel_ns"] >= marker
                     and entry.get("teardown_started_ns") == marker
                     and entry.get("error_side") == "client" and entry.get("error_errno") == errno.ECONNRESET
                     and isinstance(tcp_pending, list) and len(tcp_pending) == 2
                     and all(integer_number(value) == 0 for value in tcp_pending)
                     and isinstance(forwarded, list) and isinstance(received, list)
                     and len(forwarded) == len(received) == 2 and forwarded == received)
            if valid:
                exempt.append(entry)
            else:
                errors.append("TCP teardown-reset ledger row lacks matching zero-pending client-cancel proof")
        resets = integer_number(tcp.get("teardown_client_resets"))
        if len(tcp_error_rows) != integer_number(tcp.get("socket_errors")) or len(exempt) != resets or len(tcp_error_rows) - len(exempt) != integer_number(tcp.get("active_socket_errors")):
            errors.append("TCP close ledger does not reconcile total/active/teardown error counts")
        if resets and (len(teardown_markers) != 1 or teardown_markers[0].get("time_ns") != marker or teardown_markers[0].get("run_id") != receipt.get("run_id")):
            errors.append("TCP reset exemption lacks one matching owned teardown marker in the ledger")
    for direction, values in selected.items():
        final = receipt.get(direction) or {}
        offers = sum(key[0] == direction for key in seen)
        delivered = sum(key[0] == direction for key in dispatched) - send_errors[direction]
        if offers != integer_number(final.get("received_datagrams")) or delivered != integer_number(final.get("delivered_datagrams")):
            errors.append(direction + " ledger counters disagree with final receipt")
        if pending[direction] != integer_number(final.get("pending_datagrams")):
            errors.append(direction + " pending relay copies disagree with final receipt")
        selected[direction]["measured_offers_pending_at_teardown"] = sum(value for key, value in remaining.items()
            if key[0] == direction and interval["begin_ns"] <= offer_records[key].get("offered_ns", -1) < interval["end_ns"])
        if selected[direction]["measured_offers_pending_at_teardown"]:
            errors.append(direction + " measured relay offers remain pending at teardown")
        if drops[direction] != sum(final.get(key, 0) for key in ("random_drops", "queue_drops", "blackout_drops")) or send_errors[direction] != final.get("socket_send_errors"):
            errors.append(direction + " relay drop/error ledger does not reconcile")
        values["lateness_ms"] = timing_summary(values.pop("lateness"))
        if not values["offers"] or not values["dispatches"]:
            errors.append(direction + " has no exact measured-phase UDP offer/dispatch evidence")
        if values["lateness_ms"]["p99"] is None or values["lateness_ms"]["p99"] > timing_budget_ms:
            errors.append(direction + " measured relay dispatch p99 is unknown or exceeds budget")
    if not selected["s2c"]["data_dispatches"]:
        errors.append("no measured server-to-client UDP DATA dispatches; graphics routing remains unproved")
    return selected, list(dict.fromkeys(errors))


def actual_conditions(lab, variant):
    effective = child_environment(lab, variant.get("env"))
    relevant = {name: value for name, value in effective.items()
                if name.startswith(("FREERDP_", "SDL_", "WLOG_", "WINPR_", "DYLD_", "RDP_BENCH_"))
                and name != "FREERDP_SDL_RENDER_METRICS"}
    return {"arguments": lab["rdp_arguments"] + variant.get("arguments", []),
            "environment": relevant, "render_metrics_enabled": True,
            "certificate_store_policy": "fresh private XDG_CONFIG_HOME; observed SHA256 callback required"}


def runtime_provenance(lab, variants):
    paths = {ROOT / "bench.py", Path(__file__), ROOT / "rdpbench/bridge.py", ROOT / "rdpbench/common.py",
             ROOT / "rdpbench/link.py", ROOT / "rdpbench/feedback.py", ROOT / "rdpbench/type1.py",
             ROOT / "macos/BenchObserver.swift", ROOT / "macos/build_observer.sh", ROOT / "windows/agent.py",
             ROOT / "windows/Collect-Counters.ps1", ROOT / "windows/Install-Task.ps1"}
    paths.update(path for path in (ROOT / "fixtures").glob("*") if path.is_file() and path.suffix in (".html", ".js"))
    commands = [lab.get("observer", {}).get("command", []), lab.get("observer", {}).get("environment_command", []),
                lab.get("capture", {}).get("command", [])]
    commands += [lab.get("network", {}).get(op + "_command", []) for op in ("apply", "status", "restore")]
    for command in commands:
        if command:
            executable = shutil.which(command[0])
            if executable:
                paths.add(Path(executable))
        paths.update(Path(value) for value in command if isinstance(value, str) and Path(value).is_file()
                     and Path(value).suffix in (".py", ".sh", ".swift", ".ps1", ".dylib"))
    for variant in variants:
        paths.add(Path(variant["client"]))
        if isinstance(variant.get("build_metadata"), str) and Path(variant["build_metadata"]).is_file():
            paths.add(Path(variant["build_metadata"]))
        for path in variant.get("libraries", []):
            if isinstance(path, str):
                paths.add(Path(path))
    files = {str(path.resolve()): sha256(path) for path in sorted(paths) if path.is_file()}
    return {"files": files, "windows_sources": {str(path.relative_to(ROOT)): sha256(path)
            for path in sorted((ROOT / "windows").glob("*")) if path.is_file() and path.suffix in (".py", ".ps1")}}


def provenance_errors(provenance):
    errors = []
    for name, expected in provenance.get("files", {}).items():
        try:
            if sha256(name) != expected:
                errors.append("runtime source/binary changed: " + name)
        except OSError:
            errors.append("runtime source/binary disappeared: " + name)
    return errors


class Observer:
    CAPACITY = 240000

    def __init__(self, command, folder, lab=None):
        self.events = []
        self.lock = threading.Lock()
        self.log = Journal(Path(folder) / "observer.jsonl")
        self.failed = None
        self.overflow = 0
        self.ready = threading.Event()
        self.stderr = open(Path(folder) / "observer.stderr", "w", encoding="utf-8")
        try:
            self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=self.stderr, text=True, bufsize=1, env=child_environment(lab))
        except BaseException:
            self.log.close()
            self.stderr.close()
            raise
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        try:
            for line in self.proc.stdout:
                event = json.loads(line)
                now = time.monotonic_ns()
                if not isinstance(event.get("t_ns"), int) or isinstance(event["t_ns"], bool):
                    raise ValueError("observer lacks an integer monotonic timestamp")
                if not -50_000_000 <= now - event["t_ns"] <= 500_000_000:
                    raise ValueError("observer clock or delivery differs from controller by over 500 ms")
                self.log.emit("observer", event=event, received_ns=now)
                with self.lock:
                    if len(self.events) < self.CAPACITY:
                        self.events.append(event)
                    else:
                        self.overflow += 1
                if event.get("type") == "observer_ready":
                    self.ready.set()
                if event.get("type") in ("observer_error", "probe_error"):
                    raise RuntimeError("observer reported " + str(event.get("error", event["type"])))
        except BaseException as error:
            self.failed = type(error).__name__ + ": " + str(error)

    def wait_ready(self, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.ready.wait(.05):
                return
            self.check_live()
        raise TimeoutError("observer never became ready")

    def check_live(self):
        if self.failed or self.proc.poll() is not None or self.overflow:
            raise RuntimeError(self.failed or "observer exited or exceeded event capacity")

    def probe(self, probe_id, scheduled_ns):
        self.proc.stdin.write(json.dumps({"op": "probe", "probe_id": probe_id, "scheduled_ns": scheduled_ns}) + "\n")
        self.proc.stdin.flush()

    def snapshot(self):
        with self.lock:
            return list(self.events)

    def close(self):
        try:
            self.proc.stdin.write('{"op":"stop"}\n')
            self.proc.stdin.flush()
            self.proc.wait(5)
        except (OSError, subprocess.SubprocessError):
            self.proc.kill()
            self.proc.wait()
        self.thread.join(5)
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.log.close()
        self.stderr.close()


class ProbeSchedule:
    """Fixed injection opportunities; late opportunities are not replayed in bursts."""

    def __init__(self, begin, end, hz, budget_ms=50, base_probe=1):
        self.begin, self.end, self.interval = begin, end, round(1e9 / hz)
        self.budget_ns = round(budget_ms * 1e6)
        self.total = max(0, math.ceil((end - begin) / self.interval))
        self.cursor = 0
        self.base_probe = base_probe

    def due(self, now):
        skipped = []
        if self.cursor >= self.total or now < self.begin + self.cursor * self.interval:
            return None, skipped
        latest = min(self.total - 1, max(0, (now - self.begin) // self.interval))
        while self.cursor < latest:
            skipped.append((self.base_probe + self.cursor, self.begin + self.cursor * self.interval))
            self.cursor += 1
        item = (self.base_probe + self.cursor, self.begin + self.cursor * self.interval)
        self.cursor += 1
        if now >= self.end or now - item[1] > self.budget_ns:
            skipped.append(item)
            return None, skipped
        return item, skipped

    def finish(self):
        skipped = [(self.base_probe + index, self.begin + index * self.interval)
                   for index in range(self.cursor, self.total)]
        self.cursor = self.total
        return skipped


def observe_for(seconds, hz, observer, client, timeline, *, base_probe=1,
                budget_ms=50, inject=True):
    begin = time.monotonic_ns()
    end = begin + round(seconds * 1e9)
    schedule = ProbeSchedule(begin, end, hz, budget_ms, base_probe)
    requested, missed = [], []
    while time.monotonic_ns() < end:
        now = time.monotonic_ns()
        client.check_live()
        if observer:
            observer.check_live()
        if inject and observer:
            item, skipped = schedule.due(now)
            for probe_id, scheduled in skipped:
                missed.append(probe_id)
                timeline.emit("probe_missed", probe_id=probe_id, scheduled_ns=scheduled)
            if item:
                probe_id, scheduled = item
                observer.probe(probe_id, scheduled)
                record = {"probe_id": probe_id, "scheduled_ns": scheduled,
                          "requested_ns": time.monotonic_ns()}
                requested.append(record)
                timeline.emit("probe_requested", **record)
        time.sleep(min(.005, max(0, (end - time.monotonic_ns()) / 1e9)))
    if inject and observer:
        for probe_id, scheduled in schedule.finish():
            missed.append(probe_id)
            timeline.emit("probe_missed", probe_id=probe_id, scheduled_ns=scheduled)
    return {"begin_ns": begin, "end_ns": end,
            "expected": schedule.total if inject and observer else None,
            "requested": requested, "missed": missed}


def measured_probes(events, interval, drain_end, run_tag, phase_id, target_fps,
                    response_timeout_s=3, idle=False):
    begin, end = interval["begin_ns"], interval["end_ns"]
    requested = {event["probe_id"]: event for event in interval["requested"]}
    ordered = sorted(events, key=lambda event: event["t_ns"])
    injections, duplicates, late = {}, [], []
    for event in ordered:
        probe_id = event.get("probe_id")
        if event.get("type") != "probe_injected" or probe_id not in requested:
            continue
        if not begin <= event["t_ns"] < end:
            late.append(probe_id)
            continue
        if probe_id in injections:
            duplicates.append(probe_id)
        else:
            injections[probe_id] = event
    timeout_ns = round(response_timeout_s * 1e9)
    markers = [event for event in ordered if event.get("type") == "marker"
               and event.get("run_tag") == run_tag and event.get("phase_id") == phase_id]
    first = {}
    for event in markers:
        probe_id = event.get("probe_id")
        injection = injections.get(probe_id)
        if injection and injection["t_ns"] <= event["t_ns"] < drain_end and event["t_ns"] <= injection["t_ns"] + timeout_ns:
            first.setdefault(probe_id, event["t_ns"])
    actual = [(received - injections[probe_id]["t_ns"]) / 1e6 for probe_id, received in first.items()]
    scheduled = [(received - requested[probe_id]["scheduled_ns"]) / 1e6 for probe_id, received in first.items()]
    lateness = [(event["t_ns"] - requested[probe_id]["scheduled_ns"]) / 1e6 for probe_id, event in injections.items()]
    progress, previous = [], None
    regressions = 0
    selected = [event for event in markers if begin <= event["t_ns"] < end]
    for event in selected:
        tick = event.get("tick")
        if tick != previous:
            regressions += previous is not None and tick < previous
            progress.append(event["t_ns"])
            previous = tick
    bounds = [begin, *progress, end]
    gaps = [(right - left) / 1e6 for left, right in zip(bounds, bounds[1:])]
    unobserved = sorted(set(injections) - set(first))
    timed_out = [probe_id for probe_id in unobserved if drain_end >= injections[probe_id]["t_ns"] + timeout_ns]
    metrics = {"input_visible_ms." + key: value for key, value in timing_summary(actual).items()}
    metrics.update({"input_scheduled_ms." + key: value for key, value in timing_summary(scheduled).items()})
    metrics.update({"injection_lateness_ms." + key: value for key, value in timing_summary(lateness).items()})
    seconds = (end - begin) / 1e9
    metrics.update({"progress.observed_hz": None if idle else len(progress) / seconds,
                    "progress.target_delivery_fraction": None if idle else min(1, len(progress) / (seconds * target_fps)),
                    "progress.longest_gap_ms": None if idle else max(gaps),
                    "progress.freeze_fraction_over_250ms": None if idle else sum(gap for gap in gaps if gap > 250) / (seconds * 1000),
                    "progress.marker_observations": len(selected),
                    "progress.tick_regressions": regressions})
    probes = {"expected": interval["expected"], "requested": len(requested), "injected": len(injections),
              "observed": len(first), "unobserved": len(unobserved), "timeouts": len(timed_out),
              "missed_injections": len(interval["missed"]) + len(set(requested) - set(injections)),
              "controller_missed": len(interval["missed"]), "unconfirmed_requests": sorted(set(requested) - set(injections)),
              "observed_ids": sorted(first),
              "unobserved_ids": unobserved, "timed_out_ids": timed_out,
              "censored_ids": sorted(set(unobserved) - set(timed_out)),
              "duplicate_injection_ids": duplicates, "injections_outside_measurement_ids": late}
    return metrics, probes


def bridge_metrics(events, begin, end):
    # Receipt-time grouping is diagnostic. Intervals overlap the boundary;
    # current bridge messages do not expose phase-aligned cumulative bytes.
    samples = [(received, event["performance"]) for received, event in events
               if event.get("type") == "performance_sample" and isinstance(event.get("performance"), dict)]
    relevant = [(received, sample) for received, sample in samples if begin <= received < end
                and sample.get("connected") is True and not sample.get("reset")]
    result = {"traffic.diagnostic_sample_count": len(relevant), "verified_goodput_mbps": None}
    for name, source in (("udp_rx", "udpReceive"), ("udp_tx", "udpSend"),
                         ("tcp_rx", "tcpReceive"), ("tcp_tx", "tcpSend")):
        values = [(sample.get("traffic", {}).get(source), sample.get("intervalSeconds")) for _, sample in relevant]
        valid = [(value, seconds) for value, seconds in values if isinstance(value, (int, float))
                 and not isinstance(value, bool) and math.isfinite(value) and isinstance(seconds, (int, float))
                 and math.isfinite(seconds) and seconds > 0]
        result["traffic." + name + "_Bps_mean"] = sum(value * seconds for value, seconds in valid) / sum(seconds for _, seconds in valid) if valid else None
    sequence = [(integer_number(sample.get("generation")), integer_number(sample.get("sequence")))
                for _, sample in samples]
    known = [pair for pair in sequence if pair[1] is not None]
    result["telemetry.sequence_known_samples"] = len(known)
    result["telemetry.sequence_unknown_samples"] = len(sequence) - len(known)
    result["telemetry.sequence_gaps"] = (sum(max(0, right[1] - left[1] - 1)
        for left, right in zip(known, known[1:]) if left[0] == right[0]) if known else None)
    rtts, previous = [], None
    for _, sample in relevant:
        reading = sample.get("rtt", {})
        value, age = reading.get("milliseconds"), reading.get("ageSeconds")
        stamp = sample.get("timestamp")
        if reading.get("source") != "rdp" or not isinstance(value, (int, float)) or not isinstance(age, (int, float)) or not isinstance(stamp, (int, float)):
            continue
        key = (sample.get("epoch"), reading.get("transport"), stamp - age)
        # Snapshot age is millisecond-quantized. Infer one fresh reading only
        # when its estimated timestamp changes by more than that quantization.
        fresh = previous is None or key[:2] != previous[:2] or abs(key[2] - previous[2]) > .002
        if fresh and math.isfinite(value) and value >= 0:
            rtts.append(value)
            previous = key
    result.update({"transport_rtt_ms." + key: value for key, value in timing_summary(rtts).items()})
    reliability = [sample.get("udpReliability", {}) for _, sample in relevant]
    for field in ("retransmissionsTotal", "lossReportsTotal"):
        values = [entry[field] for entry in reliability if isinstance(entry.get(field), (int, float))]
        result["udp." + field] = values[-1] if values else None
    negotiated = {"transport": sorted({sample.get("transport") for _, sample in relevant if sample.get("transport")}),
                  "codec": sorted({sample.get("decode", {}).get("codec") for _, sample in relevant if sample.get("decode", {}).get("codec")}),
                  "graphics_udp_verified": None}
    return result, negotiated


def render_metrics(path, begin, end):
    redraws, pacing, bytes_uploaded, seconds = [], [], 0, 0
    if Path(path).exists():
        for entry in rows(path):
            if entry.get("schema") != "freerdp.sdl_render_metrics":
                continue
            start = entry.get("interval_start_ns", 0)
            finish = start + entry.get("interval_duration_ns", 0)
            # Keep complete intervals only; startup/drain samples cannot leak
            # into approximate render percentiles for the measurement phase.
            if begin <= start and finish <= end:
                redraws.extend(value / 1e6 for value in entry.get("redraw_wall_ns_samples", []) if isinstance(value, (int, float)) and value >= 0)
                pacing.extend(value / 1e6 for value in entry.get("frame_interval_wall_ns_samples", []) if isinstance(value, (int, float)) and value >= 0)
                bytes_uploaded += entry.get("uploaded_bytes", 0)
                seconds += entry.get("interval_duration_ns", 0) / 1e9
    result = {"render.redraw_ms." + key: value for key, value in timing_summary(redraws).items()}
    result.update({"render.present_interval_ms." + key: value for key, value in timing_summary(pacing).items()})
    result["render.uploaded_Bps"] = bytes_uploaded / seconds if seconds else None
    result["render.complete_interval_seconds"] = seconds
    return result


def source_evidence(folder, run_tag, phase_id, identity):
    path = Path(folder) / "windows" / "agent.jsonl"
    result = {"collected": path.is_file(), "identity_consistent": None,
              "frame_intents": None, "source_skipped_ticks": None, "dropped_events": None,
              "probe_received_ids": [], "viewport": None, "browser_errors": [], "hidden_during_phase": False,
              "event_scope": "source phase 2 through source stop, including drain/control time; source clock is not aligned to host measurement boundaries"}
    if not path.is_file():
        return result
    frames, dropped, skipped, probes, consistent = 0, 0, 0, set(), True
    for entry in rows(path):
        consistent = consistent and same_identity(identity, entry.get("identity", {}))
        if entry.get("type") in ("browser_page_error", "fixture_read_error"):
            result["browser_errors"].append(entry.get("error", entry["type"]))
        if entry.get("type") == "fixture_events":
            dropped += entry.get("droppedEvents", 0)
            for event in entry.get("events", []):
                if event.get("runTag") != run_tag or event.get("phaseId") != phase_id:
                    continue
                if event.get("type") in ("frame_intent", "video_callback"):
                    frames += 1
                    skipped = max(skipped, event.get("sourceSkippedTicks", 0))
                if event.get("type") == "probe_received":
                    probes.add(event["probeId"])
                if event.get("type") == "phase_begin":
                    result["viewport"] = event.get("viewport")
                    result["hidden_during_phase"] |= event.get("visible") != "visible"
                if event.get("type") == "visibility":
                    result["hidden_during_phase"] |= event.get("state") != "visible"
    result.update(identity_consistent=consistent, frame_intents=frames,
                  source_skipped_ticks=skipped, dropped_events=dropped,
                  probe_received_ids=sorted(probes))
    return result


def validate_inputs(cfg, lab, variants, profiles):
    for name in ("warmup_s", "measure_s", "response_timeout_s", "probe_hz", "source_fps", "scroll_px_s"):
        value = cfg.get(name, 3 if name == "response_timeout_s" else None)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError("invalid live configuration " + name)
    if cfg["probe_hz"] > 20 or cfg["measure_s"] > 3600:
        raise ValueError("live run exceeds supported observation limits")
    if not cfg.get("seeds") or not cfg.get("cases"):
        raise ValueError("live suite needs seeds and cases")
    if any(isinstance(seed, bool) or not isinstance(seed, int) or seed < 0 for seed in cfg["seeds"]) or len(set(cfg["seeds"])) != len(cfg["seeds"]):
        raise ValueError("live seeds must be unique nonnegative integers")
    if len({case.get("id") for case in cfg["cases"]}) != len(cfg["cases"]):
        raise ValueError("live case IDs must be unique")
    for case in cfg["cases"]:
        if case.get("scene") not in SCENES or case.get("profile") not in profiles:
            raise ValueError("invalid live scene/profile")
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", case.get("id", "")):
            raise ValueError("unsafe live case ID")
    for name in ("lab_id", "ssh_target", "windows_root", "windows_task", "username", "password_env"):
        if not isinstance(lab.get(name), str) or not lab[name]:
            raise ValueError("missing live lab " + name)
    if lab["ssh_target"].startswith("-") or any(char.isspace() for char in lab["ssh_target"]):
        raise ValueError("unsafe SSH target")
    if not fingerprint(lab.get("certificate_fingerprint", lab.get("certificate_sha256", ""))):
        raise ValueError("a verified SHA256 certificate fingerprint is required")
    if not isinstance(lab.get("rdp_arguments"), list):
        raise ValueError("missing RDP arguments")
    def check_arguments(arguments):
        if not isinstance(arguments, list) or any(not isinstance(value, str) for value in arguments):
            raise ValueError("live RDP arguments must be strings")
        if any(re.match(r"^(?:[/-](?:p|password)(?::|$)|--password(?:=|$))", value, re.I) for value in arguments):
            raise ValueError("passwords must be supplied through the bridge environment")
        if len(arguments) > 256 or any(not managed_argument(value) for value in arguments):
            raise ValueError("unsupported managed RDP arguments; configure displays/TLS through bridge fields and logging through WLOG environment")
    check_arguments(lab["rdp_arguments"])
    if len({variant.get("id") for variant in variants}) != len(variants):
        raise ValueError("live variant IDs must be unique")
    for variant in variants:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", variant.get("id", "")):
            raise ValueError("unsafe live variant ID")
        if not Path(variant.get("client", "")).is_file():
            raise ValueError("live client binary is missing")
        check_arguments(variant.get("arguments", []))
        if len(lab["rdp_arguments"]) + len(variant.get("arguments", [])) > 256:
            raise ValueError("too many managed RDP arguments")
        for name in variant.get("env", {}):
            if sensitive_env_name(name) or name == lab["password_env"]:
                raise ValueError("secrets must not appear in variant environment")


def trial(variant, case, profile, cfg, lab, seed, folder, expected_provenance=None, run_tag=None):
    folder = Path(folder).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    os.chmod(folder, 0o700)
    run_id = folder.name
    # Display identity must not repeat merely because two trials share a seed.
    # Otherwise a delayed old marker could falsely satisfy the new preflight.
    tag = run_tag if run_tag is not None else uuid.uuid4().int % 65535 + 1
    timeline = Journal(folder / "timeline.jsonl")
    client = observer = capture = None
    capture_output = None
    agent_attempted = applied = False
    issues, failures, notes, receipts = [], [], [], []
    interval = identity = None
    metrics, probes, negotiated, source = {}, {}, {}, {}
    complete = False
    network_budget = lab.get("network", {}).get("timing_budget_ms", 1.5)
    network_delta = network_ledger = {}
    measure_network_before = measure_network_after = final_network = None
    teardown_evidence = {}
    provenance = expected_provenance or runtime_provenance(lab, [variant])
    invariants = {"lab_id": lab["lab_id"], "profile": profile, "profile_id": case["profile"],
                  "case": case, "configuration": cfg, "client_conditions": actual_conditions(lab, variant),
                  "display_selections": lab.get("display_selections", []),
                  "observer": lab.get("observer"), "network_adapter": lab.get("network"),
                  "fixtures": {path.name: sha256(path) for path in sorted((ROOT / "fixtures").glob("*"))
                               if path.is_file() and path.suffix in (".js", ".html")}}
    manifest = {"schema": "freerdp.live_trial", "version": 1, "suite": "type2",
                "trial_id": run_id, "case_id": case["id"], "workload": case["scene"],
                "profile_id": case["profile"], "variant": variant["id"], "seed": seed,
                "utc": environment()["utc"], "commit": variant.get("commit"),
                "binary_sha256": sha256(variant["client"]), "client": str(Path(variant["client"]).resolve()),
                "client_build": variant.get("build_metadata"), "build_options": variant.get("build_options"),
                "variant_environment": variant.get("env", {}), "variant_arguments": variant.get("arguments", []),
                "comparison_key": digest(invariants), "invariants": invariants,
                "environment": environment(), "run_tag": tag, "runtime_provenance": provenance,
                "certificate_policy": "fresh private XDG_CONFIG_HOME and observed callback SHA256 pin"}
    write_json(folder / "manifest.json", manifest)
    try:
        changed = provenance_errors(provenance)
        if changed:
            raise RuntimeError("; ".join(changed))
        if variant.get("binary_sha256") and variant["binary_sha256"] != manifest["binary_sha256"]:
            raise RuntimeError("frozen client hash differs from declared variant")
        applied = True  # An adapter can mutate and then fail: restoration is still required.
        initial_network = network(lab, "apply", run_id, timeline, profile_id=case["profile"],
                                profile=profile, seed=seed, trial_dir=str(folder),
                                lease_s=cfg["warmup_s"] + cfg["measure_s"] + cfg.get("response_timeout_s", 3) + lab.get("connect_timeout_s", 90) + 180)
        receipts.append(initial_network)
        errors = receipt_errors(initial_network, "apply", profile, seed, network_budget)
        if errors:
            raise RuntimeError("network apply evidence: " + "; ".join(errors))
        tls_path = None
        if lab.get("tls_keylog") or lab.get("capture", {}).get("command"):
            tls_path = folder / "tls-secrets.txt"
            descriptor = os.open(tls_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            os.close(descriptor)
        if lab.get("capture", {}).get("command"):
            command = expanded_command(lab["capture"]["command"], folder, run_id)
            capture_output = open(folder / "capture.stderr", "w", encoding="utf-8")
            capture = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                       stderr=capture_output, env=child_environment(lab))
            time.sleep(lab.get("capture", {}).get("startup_s", .5))
            if capture.poll() is not None:
                raise RuntimeError("packet capture exited before client launch")
        actual_lab = dict(lab, rdp_arguments=lab["rdp_arguments"] + variant.get("arguments", []))
        actual_env = dict(variant.get("env", {}), FREERDP_SDL_RENDER_METRICS=str(folder / "render.jsonl"))
        client = Client(variant["client"], actual_lab, folder, actual_env, tls_path)
        client.wait_connected(lab.get("connect_timeout_s", 90))
        client.require_observed_pin()
        agent_attempted = True
        hello = agent(lab, "hello", run_id, trigger=True, agent_config=lab.get("agent_config", {}),
                      fixture_sha256=invariants["fixtures"], source_sha256=provenance["windows_sources"])
        identity = hello.get("identity", {})
        if not same_identity(identity, identity) or identity.get("windows_session_id") == 0:
            raise RuntimeError("agent has no verified interactive-session identity")
        write_json(folder / "windows-identity.json", hello)
        if hello.get("answer", {}).get("source_sha256") != provenance["windows_sources"]:
            raise RuntimeError("Windows agent source attestation is missing or mismatched")
        invariants["source_runtime"] = {"ready": hello["answer"], "windows_version": identity.get("windows_version")}
        manifest["comparison_key"] = digest(invariants)
        write_json(folder / "manifest.json", manifest)
        command = lab.get("observer", {}).get("command")
        if command:
            observer = Observer(expanded_command(command, folder, run_id, client.proc.pid), folder, lab)
            observer.wait_ready(lab.get("observer", {}).get("ready_timeout_s", 10))
        else:
            issues.append("missing independent input/marker observer")

        def start(phase):
            return agent(lab, "start", run_id, configuration={"scene": case["scene"], "phaseId": phase,
                         "runTag": tag, "seed": seed, "targetFps": cfg["source_fps"],
                         "scrollPixelsPerSecond": cfg["scroll_px_s"], "videoFps": cfg.get("source_video_fps", 30),
                         "markers": case["scene"] != "static-idle", "videoUrl": lab.get("agent_config", {}).get("video_url")},
                         video_sha256=lab.get("agent_config", {}).get("video_sha256"))

        start(1)
        client.send("focus")
        timeline.emit("warmup_begin", run_tag=tag, phase_id=1)
        observe_for(cfg["warmup_s"], cfg["probe_hz"], observer, client, timeline,
                    base_probe=100000, inject=False)
        warmup_network = network(lab, "status", run_id, timeline, profile_id=case["profile"])
        receipts.append(warmup_network)
        issues.extend("warmup network: " + error for error in receipt_errors(warmup_network, "status", profile, seed, network_budget))
        if observer and case["scene"] != "static-idle" and not any(event.get("type") == "marker" and event.get("run_tag") == tag and event.get("phase_id") == 1 for event in observer.snapshot()):
            raise RuntimeError("no CRC-valid warmup marker; ROI/source must be calibrated before measurement")
        if observer and case["scene"] != "static-idle":
            preflight_id = 0x10000000
            scheduled = time.monotonic_ns()
            observer.probe(preflight_id, scheduled)
            timeline.emit("preflight_probe_requested", probe_id=preflight_id, scheduled_ns=scheduled)
            deadline = time.monotonic() + lab.get("observer", {}).get("input_preflight_timeout_s", 3)
            while time.monotonic() < deadline:
                client.check_live()
                observer.check_live()
                if any(event.get("type") == "marker" and event.get("run_tag") == tag and event.get("phase_id") == 1 and event.get("probe_id") == preflight_id and event["t_ns"] >= scheduled for event in observer.snapshot()):
                    timeline.emit("preflight_input_verified", probe_id=preflight_id)
                    break
                time.sleep(.01)
            else:
                raise RuntimeError("preflight input did not return visibly; calibrate focus, permissions and keyboard layout")
        start(2)
        client.send("focus")
        measure_network_before = network(lab, "status", run_id, timeline, profile_id=case["profile"])
        receipts.append(measure_network_before)
        issues.extend("measurement start network: " + error for error in receipt_errors(measure_network_before, "status", profile, seed, network_budget))
        timeline.emit("measurement_begin", run_tag=tag, phase_id=2)
        interval = observe_for(cfg["measure_s"], cfg["probe_hz"], observer, client, timeline,
                               budget_ms=cfg.get("injection_budget_ms", 50), inject=case["scene"] != "static-idle")
        timeline.emit("measurement_end", end_ns=interval["end_ns"])
        measure_network_after = network(lab, "status", run_id, timeline, profile_id=case["profile"])
        receipts.append(measure_network_after)
        issues.extend("measurement end network: " + error for error in receipt_errors(measure_network_after, "status", profile, seed, network_budget))
        network_delta, errors = udp_activity_delta(measure_network_before, measure_network_after)
        issues.extend(errors)
        drain_end = interval["end_ns"] + round(cfg.get("response_timeout_s", 3) * 1e9)
        while time.monotonic_ns() < drain_end:
            client.check_live()
            if observer:
                observer.check_live()
            time.sleep(.01)
        timeline.emit("response_drain_end", end_ns=drain_end)
        drained_network = network(lab, "status", run_id, timeline, profile_id=case["profile"])
        receipts.append(drained_network)
        issues.extend("drained network: " + error for error in receipt_errors(drained_network, "status", profile, seed, network_budget))
        if observer:
            metrics, probes = measured_probes(observer.snapshot(), interval, drain_end, tag, 2,
                                             cfg.get("source_video_fps", 30) if case["scene"] == "video" else cfg["source_fps"],
                                             cfg.get("response_timeout_s", 3), case["scene"] == "static-idle")
            if probes["missed_injections"] or probes["duplicate_injection_ids"] or probes["censored_ids"]:
                issues.append("input injection or response-drain coverage is incomplete")
            if probes["timeouts"]:
                failures.append("injected input responses timed out")
            if case["scene"] != "static-idle" and not metrics["progress.marker_observations"]:
                issues.append("no CRC-valid markers observed during measurement")
            if metrics["input_visible_ms.count"] < 100 and case["scene"] != "static-idle":
                notes.append("Fewer than 100 observed responses; p99 is a sparse diagnostic, not a stable tail estimate.")
        status = agent(lab, "status", run_id)
        write_json(folder / "windows-final-status.json", status)
        if not same_identity(identity, status.get("identity", {})):
            failures.append("Windows interactive agent/session identity changed")
        if status.get("answer", {}).get("source_sha256") != provenance["windows_sources"] or status.get("answer", {}).get("fixture_sha256") != invariants["fixtures"]:
            issues.append("Windows agent/fixture source hashes changed or became unknown")
        if not status.get("answer", {}).get("active"):
            failures.append("Windows source workload was not active at measurement end")
        traffic, negotiated = bridge_metrics(client.snapshot(), interval["begin_ns"], interval["end_ns"])
        metrics.update(traffic)
        if not metrics["traffic.diagnostic_sample_count"]:
            issues.append("no connected bridge performance samples during measurement")
        if capture and capture.poll() is not None:
            issues.append("packet capture stopped during measurement")
        complete = True
    except Exception as error:
        issues.append(type(error).__name__ + ": " + str(error))
        timeline.emit("trial_error", error=issues[-1])
    finally:
        if agent_attempted:
            try:
                agent(lab, "exit", run_id, timeout=20)
            except Exception as error:
                issues.append("Windows agent cleanup: " + type(error).__name__)
                timeline.emit("agent_cleanup_error", error=str(error))
            try:
                collect_windows(lab, run_id, folder)
            except Exception as error:
                issues.append("Windows log collection: " + type(error).__name__)
                timeline.emit("windows_collection_error", error=str(error))
        for name, producer in (("observer", observer), ("client", client)):
            if producer:
                if name == "client" and applied:
                    try:
                        if client.proc.poll() is not None:
                            issues.append("client exited before owned teardown began")
                        else:
                            teardown_evidence, errors = begin_owned_teardown(lab, run_id, case["profile"], profile,
                                seed, timeline, receipts, network_budget)
                            issues.extend("teardown network: " + error for error in errors)
                    except Exception as error:
                        issues.append("owned teardown marker failed: " + type(error).__name__)
                        timeline.emit("teardown_marker_error", error=str(error))
                    teardown_evidence["client_cancel_ns"] = time.monotonic_ns()
                    timeline.emit("owned_client_cancel", **teardown_evidence)
                try:
                    producer.close()
                except Exception as error:
                    issues.append(name + " cleanup failed: " + type(error).__name__)
                    timeline.emit("producer_cleanup_error", producer=name, error=str(error))
        if capture:
            try:
                capture.terminate()
                try:
                    capture.wait(10)
                except subprocess.TimeoutExpired:
                    capture.kill()
                    capture.wait()
            except Exception as error:
                issues.append("capture cleanup failed: " + type(error).__name__)
            finally:
                capture_output.close()
        if applied:
            try:
                final_network = network(lab, "restore", run_id, timeline, profile_id=case["profile"])
                receipts.append(final_network)
                issues.extend("restored network: " + error for error in receipt_errors(final_network, "restore", profile, seed, network_budget, teardown_evidence))
            except Exception as error:
                issues.append("network restore failed: " + type(error).__name__)
                timeline.emit("restore_error", error=str(error))
        timeline.close()
    if final_network and interval:
        try:
            network_ledger, errors = ledger_evidence(final_network, interval, folder, network_budget, teardown_evidence)
            issues.extend(errors)
        except Exception as error:
            issues.append("closed network ledger audit: " + type(error).__name__)
    issues.extend(provenance_errors(provenance))
    if interval:
        try:
            metrics.update(render_metrics(folder / "render.jsonl", interval["begin_ns"], interval["end_ns"]))
        except Exception as error:
            issues.append("render evidence: " + type(error).__name__)
    if identity:
        try:
            source = source_evidence(folder, tag, 2, identity)
            if not source["collected"] or not source["identity_consistent"] or source["dropped_events"]:
                issues.append("source events missing, changed identity, or overflowed")
            if source["browser_errors"] or source["hidden_during_phase"]:
                issues.append("Windows browser reported an error or hidden workload")
            if case["scene"] != "static-idle" and source["frame_intents"] == 0:
                issues.append("Windows source produced no animation events in the measured phase")
            if set(probes.get("observed_ids", [])) - set(source["probe_received_ids"]):
                issues.append("observed input IDs are absent from collected source events")
        except Exception as error:
            issues.append("source evidence: " + type(error).__name__)
    try:
        if sha256(variant["client"]) != manifest["binary_sha256"]:
            issues.append("client binary changed during trial")
    except OSError:
        issues.append("client binary missing at trial end")
    notes += ["Input latency ends at compositor capture callback; it does not measure physical photon time.",
              "Traffic rates include protected datagrams/retransmissions and have receipt-time phase boundaries; they are not verified application goodput.",
              "Marker validity proves only the marker region. Full-image fidelity and graphics routing require separate capture review.",
              "Transport RTT values deduplicate recent RDP readings; they remain diagnostic and are separate from numbered input probes."]
    udp_only = any(receipt.get("full_rdp_profile") is False or receipt.get("tcp_impaired") is False for receipt in receipts)
    if udp_only:
        notes.append("UDP-only seeded network treatment; TCP control and fallback remain unshaped. These are not full-RDP 10 Mbit/s link results.")
    result = {"schema": "freerdp.live_trial", "version": 1, "suite": "type2", "trial_id": run_id,
              "phase": cfg.get("phase", "main"), "case_id": case["id"], "workload": case["scene"],
              "profile_id": case["profile"], "variant": variant["id"], "seed": seed,
              "utc": manifest["utc"], "commit": manifest["commit"], "binary_sha256": manifest["binary_sha256"],
              "comparison_key": manifest["comparison_key"],
              "validity": "PARTIAL_E2E_EVIDENCE" if issues else "PROVISIONAL_LIVE",
              "sut_outcome": "FAIL" if failures else "COMPLETE" if complete else "INCOMPLETE",
              "strict_eligible": complete and not issues and not failures,
              "integrity_verified": False, "metrics": metrics, "probes": probes, "negotiated": negotiated,
              "source": source, "windows_identity": identity, "network_receipts": receipts,
              "network_scope": "UDP_ONLY_TCP_UNSHAPED" if udp_only else "ADAPTER_DECLARED_SCOPE",
              "network_udp_packet_delta": network_delta, "network_measured_ledger": network_ledger,
              "owned_teardown": teardown_evidence,
              "runtime_provenance": provenance, "observed_certificate_pin": client.pin_fingerprint if client else None,
              "certificate_policy": manifest["certificate_policy"],
              "reasons": issues, "sut_failures": failures, "notes": notes,
              "artifacts": {"root": str(folder), "manifest": "manifest.json", "bridge": "bridge.jsonl",
                            "source": "windows/agent.jsonl", "observer": "observer.jsonl", "render": "render.jsonl"}}
    write_json(folder / "summary.json", result)
    seal(folder)
    return result


def aggregate(base, plan, lab):
    trials = []
    for entry in plan["trials"]:
        folder = Path(base) / entry["trial_id"]
        if not (folder / "summary.json").exists():
            preflight = plan.get("environment_preflight", {})
            reasons = ["planned trial has no completed summary"]
            if preflight.get("required") and preflight.get("available") is not True:
                reasons += ["desktop preflight: " + reason for reason in preflight.get("reasons", [])]
            trials.append(dict(entry, validity="INCOMPLETE", sut_outcome="INCOMPLETE", strict_eligible=False,
                               reasons=reasons, metrics={}, probes={}))
            continue
        result = read_json(folder / "summary.json")
        errors = audit(folder)
        if errors:
            result.update(validity="INVALID_ARTIFACT", strict_eligible=False,
                          metrics={}, probes={}, reasons=result.get("reasons", []) + errors)
        trials.append(result)
    return {"schema": "freerdp.live_benchmark", "version": 1, "scope": "live Windows RDP",
            "campaign_id": plan["campaign_id"], "provenance": {"lab_id": lab["lab_id"], "variants": plan["variants"],
            "host": plan["environment"], "runtime": plan.get("runtime_provenance"), "vm_os": next((trial.get("windows_identity", {}).get("windows_version") for trial in trials if trial.get("windows_identity")), None)},
            "network_profiles": plan["profiles"], "calibration": lab.get("calibration", {"status": "provisional"}),
            "comparison_variants": plan.get("comparison_variants"), "trials": trials,
            "environment_preflight": plan.get("environment_preflight"),
            "notes": ["Live interaction, render timings and transport-only goodput are separate measurements.",
                      "UDP-only proxy profiles leave TCP control/fallback unshaped; inspect each trial's network_scope.",
                      "All planned trials, failures and incomplete evidence remain visible."]}


def run_suite(args):
    cfg, lab = read_json(args.suite), read_json(args.lab)
    variants = read_json(args.variants)["variants"]
    profiles = read_json(args.profiles)["profiles"]
    validate_inputs(cfg, lab, variants, profiles)
    cases = [case for case in cfg["cases"] if not args.case or case["id"] == args.case]
    if not cases:
        raise ValueError("no matching live case")
    base = Path(args.output).resolve()
    base.mkdir(parents=True, exist_ok=False)
    os.chmod(base, 0o700)
    plan = {"version": 1, "campaign_id": base.name, "environment": environment(), "variants": variants,
            "profiles": [{"id": name, **profiles[name]} for name in sorted({case["profile"] for case in cases})],
            "comparison_variants": cfg.get("comparison_variants"), "trials": []}
    plan["runtime_provenance"] = runtime_provenance(lab, variants)
    for case in cases:
        for seed in cfg["seeds"]:
            order = list(variants)
            random.Random(seed).shuffle(order)
            for variant in order:
                name = f"{case['id']}-{variant['id']}-s{seed}-{uuid.uuid4().hex[:8]}"
                plan["trials"].append({"trial_id": name, "case_id": case["id"], "workload": case["scene"],
                                       "profile_id": case["profile"], "variant": variant["id"], "seed": seed})
    if len(plan["trials"]) > 65535:
        raise ValueError("campaign exceeds the unique visual tag space")
    for entry, tag in zip(plan["trials"], random.SystemRandom().sample(range(1, 65536), len(plan["trials"]))):
        entry["run_tag"] = tag
    write_json(base / "campaign.json", plan)
    write_json(base / "live-results.json", aggregate(base, plan, lab))
    if not record_environment_preflight(lab, base, plan):
        print("LIVE campaign stopped before launch: desktop environment unavailable; see preflight-environment.json", flush=True)
        return 2
    failed = False
    try:
        for index, entry in enumerate(plan["trials"]):
            if index and not record_environment_preflight(lab, base, plan):
                print("LIVE campaign stopped before next trial: desktop environment unavailable", flush=True)
                return 2
            case = next(case for case in cases if case["id"] == entry["case_id"])
            variant = next(variant for variant in variants if variant["id"] == entry["variant"])
            print("LIVE", entry["trial_id"], flush=True)
            result = trial(variant, case, profiles[case["profile"]], cfg, lab, entry["seed"], base / entry["trial_id"],
                           plan["runtime_provenance"], entry["run_tag"])
            print(result["validity"], result["sut_outcome"], flush=True)
            failed |= result["sut_outcome"] != "COMPLETE" or bool(result["reasons"])
            write_json(base / "live-results.json", aggregate(base, plan, lab))
    finally:
        write_json(base / "live-results.json", aggregate(base, plan, lab))
    return 1 if failed else 0

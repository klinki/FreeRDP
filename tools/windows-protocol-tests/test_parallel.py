"""Process-level regressions for independent runner listeners and cleanup."""

import errno
import json
import os
from pathlib import Path
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

from control import Client, ControlServer, stop_process
from listener import ListenerHandshake
from profiles import CaseOptions
from run import create_output_directory

HERE = Path(__file__).resolve().parent


def line(process, timeout=10):
    if not select.select([process.stdout], [], [], timeout)[0]:
        raise AssertionError("Worker did not publish its readiness before the deadline")
    value = process.stdout.readline()
    if not value:
        raise AssertionError(f"Worker exited without readiness: {process.poll()}")
    return json.loads(value)


def parameters(port):
    address = b"127.0.0.1"
    return struct.pack("<I6H", 1, port, 0, 1024, 768, 0, len(address)) + address


# All processes retain their actual bound socket until the parent releases them.
# A readiness check must not create a TCP connection: that would make the first
# select() below readable and consume a protocol test's expected connection.
LISTENER_WORKER = r'''
import json, os, select, socket, sys
from pathlib import Path
from control import ControlServer
from listener import FILE_ENV, TOKEN_ENV
from run import create_output_directory
output = create_output_directory(root=Path(sys.argv[1]))
listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen(1)
control = ControlServer(None)
record = dict(version=1, token=os.environ[TOKEN_ENV], address="127.0.0.1",
              port=listener.getsockname()[1], pid=os.getpid())
path = Path(os.environ[FILE_ENV])
temporary = path.with_suffix(".tmp")
temporary.write_text(json.dumps(record))
temporary.replace(path)
print(json.dumps(dict(output=str(output), control_port=control.server_address[1])), flush=True)
sys.stdin.readline()
print(json.dumps(dict(probed=bool(select.select([listener], [], [], 0)[0]))), flush=True)
listener.close()
control.server_close()
'''


class ListenerTests(unittest.TestCase):
    def record(self, handshake, **updates):
        value = dict(version=1, token=handshake.token, address="127.0.0.1",
                     port=3391, pid=os.getpid())
        value.update(updates)
        handshake.path.write_text(json.dumps(value))
        handshake.driver_group = os.getpgrp()

    def test_record_rejects_stale_or_unowned_nonloopback_endpoints(self):
        cases = [dict(token="wrong"), dict(token=None), dict(token=42), dict(token="\u0100"),
                 dict(address="0.0.0.0"), dict(address="localhost"),
                 dict(port=0), dict(port=65536), dict(port=True), dict(pid=0),
                 dict(version=True), dict(version=2)]
        with tempfile.TemporaryDirectory() as temporary:
            for index, fields in enumerate(cases):
                with self.subTest(fields=fields):
                    handshake = ListenerHandshake(Path(temporary) / f"record-{index}.json")
                    self.record(handshake, **fields)
                    with self.assertRaises(ValueError):
                        handshake.read()
            handshake = ListenerHandshake(Path(temporary) / "unowned.json")
            self.record(handshake)
            handshake.driver_group = os.getpgrp() + 1
            with self.assertRaisesRegex(ValueError, "does not belong"):
                handshake.read()

    def test_missing_and_nonregular_records_fail_without_waiting(self):
        with tempfile.TemporaryDirectory() as temporary:
            handshake = ListenerHandshake(Path(temporary) / "record.json")
            with self.assertRaises(FileNotFoundError):
                handshake.read()
            handshake.path.write_text("x" * 4097)
            with self.assertRaisesRegex(ValueError, "file"):
                handshake.read()
            handshake.path.unlink()
            handshake.path.symlink_to(Path(temporary) / "absent")
            with self.assertRaises(OSError):
                handshake.read()
            handshake.path.unlink()
            os.mkfifo(handshake.path)
            with self.assertRaisesRegex(ValueError, "file"):
                handshake.read()

    def test_dynamic_client_accepts_only_the_published_endpoint(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            handshake = ListenerHandshake(directory / "listener.json")
            self.record(handshake)
            client = Client(directory / "client", None, directory, "fingerprint", "standard",
                            listener=handshake)
            with self.assertRaisesRegex(ValueError, "configured loopback"):
                client.start(parameters(3392))
            self.assertEqual(client.starts, 0)
            process = Mock()
            process.poll.return_value = 0
            with patch("control.subprocess.Popen", return_value=process) as launch:
                try:
                    client.start(parameters(3391))
                    self.assertIn("/v:127.0.0.1:3391", launch.call_args.args[0])
                    self.assertEqual(client.starts, 1)
                finally:
                    client.stop()
            self.record(handshake, port=3392)
            with self.assertRaisesRegex(ValueError, "changed"):
                handshake.read()

    def test_parallel_processes_own_unique_bound_ports_and_output_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            workers = []
            records = []
            try:
                for index in range(8):
                    handshake = ListenerHandshake(directory / f"listener-{index}.json")
                    process = subprocess.Popen(
                        [sys.executable, "-c", LISTENER_WORKER, str(directory / "results")],
                        cwd=HERE, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                        env=dict(os.environ, **handshake.environment), start_new_session=True,
                    )
                    handshake.driver_group = process.pid
                    workers.append((process, handshake))
                for process, handshake in workers:
                    worker = line(process)
                    record = handshake.read()
                    records.append((worker, record))
                    self.assertEqual(record["pid"], process.pid)
                    for port in (record["port"], worker["control_port"]):
                        with socket.socket() as contender:
                            with self.assertRaises(OSError):
                                contender.bind(("127.0.0.1", port))
                    self.assertEqual(stat.S_IMODE(Path(worker["output"]).stat().st_mode), 0o700)
                self.assertEqual(len({worker["output"] for worker, _ in records}), 8)
                self.assertEqual(len({record["port"] for _, record in records}), 8)
                all_ports = [port for worker, record in records
                             for port in (worker["control_port"], record["port"])]
                self.assertEqual(len(set(all_ports)), 16)
                for process, _ in workers:
                    process.stdin.write("release\n")
                    process.stdin.flush()
                    self.assertFalse(line(process)["probed"])
                    self.assertEqual(process.wait(timeout=5), 0)
                for port in all_ports:
                    with socket.socket() as contender:
                        contender.bind(("127.0.0.1", port))
            finally:
                for process, _ in workers:
                    stop_process(process)
                    process.stdin.close()
                    process.stdout.close()

    def test_explicit_output_directory_cannot_be_reused(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "explicit"
            self.assertEqual(create_output_directory(output), output.resolve())
            with self.assertRaisesRegex(ValueError, "already exists"):
                create_output_directory(output)


FAKE_DRIVER = '''#!/usr/bin/env python3
import json, os, socket, time
from pathlib import Path
listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen(1)
record = dict(version=1, token=os.environ["FREERDP_WPTS_LISTENER_TOKEN"],
              address="127.0.0.1", port=listener.getsockname()[1], pid=os.getpid())
path = Path(os.environ["FREERDP_WPTS_LISTENER_FILE"])
temporary = path.with_suffix(".tmp")
temporary.write_text(json.dumps(record))
temporary.replace(path)
time.sleep(120)
'''

CASE_WORKER = r'''
import json, sys
from pathlib import Path
import run
from profiles import CaseOptions
root = Path(sys.argv[1])
# This test isolates runner/process cleanup; the real Microsoft adapter is
# validated separately. Each worker still owns a separate suite/config path.
run.configure = lambda suite, *args: (suite / "configured").write_text(str(args))
suite = root / "suite"
suite.mkdir()
result = run.run_case(Path("/unused"), suite, "standard", "fake", root / "case",
                      root / "cert.pfx", "fingerprint", 1, options=CaseOptions())
print(json.dumps(result), flush=True)
'''


class ParallelCleanupTests(unittest.TestCase):
    def test_timed_out_parallel_drivers_release_only_their_owned_listeners(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executables = directory / "bin"
            executables.mkdir()
            driver = executables / "dotnet"
            driver.write_text(FAKE_DRIVER)
            driver.chmod(0o700)
            environment = dict(os.environ, PATH=f"{executables}{os.pathsep}{os.environ['PATH']}")
            workers = []
            # Keep an unrelated listener open across every worker's cleanup.
            with socket.socket() as independent:
                independent.bind(("127.0.0.1", 0))
                independent.listen(1)
                try:
                    for index in range(2):
                        root = directory / f"worker-{index}"
                        root.mkdir()
                        process = subprocess.Popen(
                            [sys.executable, "-c", CASE_WORKER, str(root)], cwd=HERE,
                            stdout=subprocess.PIPE, text=True, env=environment, start_new_session=True,
                        )
                        workers.append((process, root))
                    for process, root in workers:
                        result = line(process)
                        self.assertEqual(process.wait(timeout=5), 0)
                        self.assertFalse(result["passed"])
                        self.assertIn("Driver exceeded 1s deadline", result["errors"])
                        startup = json.loads((root / "case/startup.json").read_text())
                        self.assertEqual(startup["listener"]["pid"], startup["driver_pid"])
                        self.assertEqual(startup["client_launches"], 0)
                        self.assertTrue((root / "suite/configured").is_file())
                        with self.assertRaises(ProcessLookupError):
                            os.kill(startup["driver_pid"], 0)
                        with socket.socket() as contender:
                            contender.bind(("127.0.0.1", startup["listener"]["port"]))
                        with socket.socket() as contender:
                            with self.assertRaises(OSError):
                                contender.bind(independent.getsockname())
                finally:
                    for process, _ in workers:
                        stop_process(process)
                        process.stdout.close()


class CleanupRaceTests(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, "waitid") and hasattr(os, "WNOWAIT"),
                         "Requires observing child exit without reaping it")
    def test_actual_child_exit_between_poll_and_signal_is_reaped(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import sys; sys.stdin.readline()"],
            stdin=subprocess.PIPE, text=True, start_new_session=True,
        )
        real_killpg = os.killpg
        signal_errors = []

        def exit_before_signal(pid, number):
            self.assertEqual(pid, process.pid)
            process.stdin.write("exit\n")
            process.stdin.flush()
            # This deliberately places exit after stop_process's first poll,
            # without a timing guess or reaping before the actual killpg call.
            os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOWAIT)
            try:
                return real_killpg(pid, number)
            except OSError as error:
                signal_errors.append(error.errno)
                raise

        try:
            with patch("control.os.killpg", side_effect=exit_before_signal):
                stop_process(process)
            self.assertEqual(process.returncode, 0)
            if sys.platform == "darwin":
                self.assertEqual(signal_errors, [errno.EPERM])
        finally:
            process.stdin.close()
            stop_process(process)

    def test_permission_failure_is_suppressed_only_after_reaping_exited_child(self):
        for during_kill in (False, True):
            with self.subTest(during_kill=during_kill):
                process = Mock(pid=12345)
                process.poll.return_value = None
                process.wait.return_value = 0
                if during_kill:
                    process.wait.side_effect = [subprocess.TimeoutExpired("owned", 5), 0, 0]
                    errors = [None, PermissionError(errno.EPERM, "Operation not permitted")]
                else:
                    errors = [PermissionError(errno.EPERM, "Operation not permitted")]
                with patch("control.os.killpg", side_effect=errors) as send:
                    stop_process(process)
                self.assertEqual(process.poll.call_count, 1)
                self.assertTrue(all(call.kwargs == {"timeout": 5}
                                    for call in process.wait.call_args_list))
                self.assertEqual(send.call_count, 2 if during_kill else 1)
                self.assertEqual(send.call_args.args[0], process.pid)

    def test_permission_failure_for_running_child_is_not_ignored(self):
        for during_kill in (False, True):
            with self.subTest(during_kill=during_kill):
                process = Mock(pid=12345)
                process.poll.return_value = None
                process.wait.side_effect = subprocess.TimeoutExpired("owned", 5)
                if during_kill:
                    errors = [None, PermissionError(errno.EPERM, "Operation not permitted")]
                else:
                    errors = [PermissionError(errno.EPERM, "Operation not permitted")]
                with patch("control.os.killpg", side_effect=errors):
                    with self.assertRaises(PermissionError):
                        stop_process(process)
                self.assertEqual(process.poll.call_count, 1)
                self.assertEqual(process.wait.call_count, 1 + int(during_kill))
                self.assertTrue(all(call.kwargs == {"timeout": 5}
                                    for call in process.wait.call_args_list))

    def test_exit_after_denied_signal_is_confirmed_by_bounded_wait(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import sys; sys.stdin.readline()"],
            stdin=subprocess.PIPE, text=True, start_new_session=True,
        )
        real_wait = process.wait
        observed_live = []

        def release_and_wait(*, timeout):
            # At the denied signal's first wait, the child is definitely still
            # running. Release its input gate only now; no guessed sleep races.
            if not observed_live:
                observed_live.append(process.poll() is None)
                process.stdin.write("exit\n")
                process.stdin.flush()
            self.assertEqual(timeout, 5)
            return real_wait(timeout=timeout)

        try:
            with patch("control.os.killpg", side_effect=PermissionError(errno.EPERM, "denied")), \
                    patch.object(process, "wait", side_effect=release_and_wait):
                stop_process(process)
            self.assertEqual(observed_live, [True])
            self.assertEqual(process.returncode, 0)
        finally:
            process.stdin.close()
            stop_process(process)

    def test_adapter_failure_preserves_command_and_traceback(self):
        client = Mock()
        client.options.graceful_close = False
        client.stop.side_effect = PermissionError(errno.EPERM, "Operation not permitted")
        server = ControlServer(client)
        worker = threading.Thread(target=server.handle_request)
        worker.start()
        try:
            case = b"case"
            wire = (struct.pack("<HHHI", 0, 1, 2, len(case)) + case
                    + struct.pack("<HII", 1, 0, 0))
            with socket.create_connection(server.server_address, timeout=3) as peer:
                peer.sendall(wire)
                response = peer.recv(4096)
            self.assertIn(b"Operation not permitted", response)
        finally:
            worker.join(timeout=5)
            server.server_close()
        self.assertEqual(len(server.failures), 1)
        detail = server.failure_details[0]
        self.assertEqual((detail["stage"], detail["command"], detail["exception"]),
                         ("execute-command", 2, "PermissionError"))
        self.assertIn("self.server.client.stop()", detail["traceback"])


if __name__ == "__main__":
    unittest.main()

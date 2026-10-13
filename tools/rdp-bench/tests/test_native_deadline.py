"""Campaign deadlines cancel native drain waits without becoming transport failures."""
import os
from pathlib import Path
import socket
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from rdpbench.native import Native


class NativeDeadlineTests(unittest.TestCase):
    @staticmethod
    def fixture(available=True, setter_result=1):
        driver = Native.__new__(Native)
        driver.h = object()
        driver.deadline_available = available
        driver.lib = SimpleNamespace(
            freerdp_bench_v1_deadline=Mock(return_value=setter_result),
            freerdp_bench_v1_deadline_reached=Mock(return_value=0),
        )
        driver._queue_clock_sample = Mock(return_value={"offset_ns": -300})
        return driver

    def test_deadline_is_translated_to_native_clock(self):
        driver = self.fixture()
        driver.configure_deadline(10000)
        driver.lib.freerdp_bench_v1_deadline.assert_called_once_with(driver.h, 9700)

    def test_configuration_failure_is_not_silently_ignored(self):
        driver = self.fixture(setter_result=0)
        with self.assertRaisesRegex(RuntimeError, "deadline configuration failed"):
            driver.configure_deadline(10000)

    def test_older_adapter_has_no_deadline_claim(self):
        driver = self.fixture(available=False)
        driver.configure_deadline(10000)
        self.assertFalse(driver.deadline_reached())
        driver._queue_clock_sample.assert_not_called()
        driver.lib.freerdp_bench_v1_deadline.assert_not_called()
        driver.lib.freerdp_bench_v1_deadline_reached.assert_not_called()

    def test_status_reports_observed_cancellation(self):
        driver = self.fixture()
        self.assertFalse(driver.deadline_reached())
        driver.lib.freerdp_bench_v1_deadline_reached.return_value = 1
        self.assertTrue(driver.deadline_reached())
        driver.lib.freerdp_bench_v1_deadline_reached.assert_called_with(driver.h)

    @unittest.skipUnless(os.environ.get("RDP_BENCH_TEST_LIBRARY"),
                         "set RDP_BENCH_TEST_LIBRARY for actual native deadline validation")
    def test_actual_native_read_and_send_expiration_cleanup(self):
        library = os.environ["RDP_BENCH_TEST_LIBRARY"]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(("127.0.0.1", 0))
            peer.settimeout(0.02)
            for operation in ("read", "send"):
                with self.subTest(operation=operation):
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as temporary:
                        temporary.bind(("127.0.0.1", 0))
                        port = temporary.getsockname()[1]
                    driver = Native(library, "127.0.0.1", port,
                                    "127.0.0.1", peer.getsockname()[1])
                    try:
                        self.assertTrue(driver.deadline_available)
                        native_now = driver.lib.freerdp_bench_v1_clock_ns()
                        self.assertEqual(driver.lib.freerdp_bench_v1_deadline(
                            driver.h, native_now - 1), 1)
                        self.assertFalse(driver.deadline_reached())
                        with self.assertRaisesRegex(
                                RuntimeError, "benchmark deadline reached; reliable drain incomplete"):
                            if operation == "read":
                                driver.read(0)
                            else:
                                driver.send(b"never accepted")
                        self.assertTrue(driver.deadline_reached())
                        stats = driver.stats()
                        if getattr(driver,'backend',1)==2:
                            self.assertIsNone(stats['send_failed']);self.assertIsNone(stats['receive_failed'])
                            self.assertEqual(stats['transport_failed'],0)
                        else:self.assertEqual(stats['send_failed'],1)
                        self.assertEqual(stats["sent_datagrams"], 0)
                        with self.assertRaises(socket.timeout):
                            peer.recv(2048)
                    finally:
                        driver.close()
                    self.assertIsNone(driver.h)
                    driver.close()  # Cleanup is safe after cancellation and on repeated close.
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reusable:
                        reusable.bind(("127.0.0.1", port))


if __name__ == "__main__":
    unittest.main()

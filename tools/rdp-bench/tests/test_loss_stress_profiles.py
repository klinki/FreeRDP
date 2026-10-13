"""High-loss suites keep the control and use probabilities in each direction."""
import copy
from decimal import Decimal
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from rdpbench.link import Link
from rdpbench.live import validate_inputs
from rdpbench.type1 import identifier, validate_profile


class LossStressProfilesTests(unittest.TestCase):
    def setUp(self):
        self.profiles = json.loads((ROOT / "configs/profiles.json").read_text())["profiles"]
        self.native = json.loads((ROOT / "configs/type1.loss-stress.json").read_text())
        self.live = json.loads((ROOT / "configs/type2.loss-stress.json").read_text())

    def test_exact_probabilities_and_control_are_distinct_from_percent_values(self):
        decimal_profiles = json.loads((ROOT / "configs/profiles.json").read_text(),
                                      parse_float=Decimal)["profiles"]
        for percent, probability in ((25, "0.25"), (50, "0.5"), (70, "0.7")):
            profile = self.profiles[f"ten_mbit_loss{percent}"]
            validate_profile(profile)
            for direction in ("c2s", "s2c"):
                self.assertEqual(decimal_profiles[f"ten_mbit_loss{percent}"][direction]["loss"],
                                 Decimal(probability))
        expected_control = dict(one_way_ms=15, jitter_ms=1, loss=0.0005,
                                duplicate=0, reorder=0, rate_mbps=10,
                                queue_ms=30, overhead_bytes=28)
        self.assertEqual(self.profiles["ten_mbit"]["c2s"], expected_control)
        self.assertEqual(self.profiles["ten_mbit"]["s2c"], expected_control)
        self.assertEqual(self.profiles["ten_mbit_clean"]["c2s"]["loss"], 0)
        self.assertEqual(self.profiles["ten_mbit_clean"]["s2c"]["loss"], 0)
        # The production validator catches accidentally entering 25 rather than .25.
        invalid = copy.deepcopy(self.profiles["ten_mbit_loss25"])
        invalid["s2c"]["loss"] = 25
        with self.assertRaisesRegex(ValueError, "probability"):
            validate_profile(invalid)

    def test_both_directions_change_only_loss_relative_to_mild_control(self):
        for percent in (25, 50, 70):
            profile = self.profiles[f"ten_mbit_loss{percent}"]
            self.assertEqual(profile["c2s"], profile["s2c"])
            for direction in ("c2s", "s2c"):
                parameters = dict(profile[direction])
                parameters["loss"] = self.profiles["ten_mbit"][direction]["loss"]
                self.assertEqual(parameters, self.profiles["ten_mbit"][direction])

    def test_loss_schedule_is_repeatable_independent_and_covers_requested_rates(self):
        # Spaced datagrams avoid queue overflow, so counted drops are random loss.
        # Exercise the actual shaper rather than only inspecting JSON constants.
        payload = bytes(1200)
        for percent in (25, 50, 70):
            masks = []
            for direction, seed in (("c2s", 11), ("s2c", 11 + 1000003)):
                profile = self.profiles[f"ten_mbit_loss{percent}"][direction]
                first, repeated = Link(profile, seed, direction), Link(profile, seed, direction)
                offers = [first.offer(i * 3_000_000, payload) for i in range(10000)]
                self.assertEqual(offers, [repeated.offer(i * 3_000_000, payload)
                                          for i in range(10000)])
                mask = [not offered for offered in offers]
                self.assertEqual(first.count["queue_drops"], 0)
                self.assertEqual(sum(mask), first.count["random_drops"])
                self.assertAlmostEqual(sum(mask) / len(mask), percent / 100, delta=0.02)
                masks.append(mask)
            self.assertNotEqual(masks[0], masks[1])

    def test_suites_pin_twenty_candidate_conditions_without_queue_variants(self):
        expected_profiles = ["ten_mbit", "ten_mbit_loss25", "ten_mbit_loss50", "ten_mbit_loss70"]
        for cfg in (self.native, self.live):
            self.assertEqual(cfg["seeds"], [11, 29, 47, 71, 101])
            self.assertEqual(cfg["warmup_s"], 5)
            self.assertEqual(cfg["phase"], "main")
            self.assertEqual([case["profile"] for case in cfg["cases"]], expected_profiles)
            self.assertEqual(len(cfg["cases"]) * len(cfg["seeds"]), 20)
            self.assertEqual(len({case["id"] for case in cfg["cases"]}), 4)
            self.assertNotIn("comparison_variants", cfg)
            self.assertNotIn("RDP_BENCH_SEND_QUEUE_BYTES", json.dumps(cfg))
            for case in cfg["cases"]:
                identifier(case["id"])
                validate_profile(self.profiles[case["profile"]])
        self.assertEqual(self.native["measure_s"], 30)
        self.assertEqual(self.native["drain_s"], 12)
        self.assertEqual(self.native["payload_bytes"], 16384)
        self.assertEqual(self.native["probe_hz"], 20)
        self.assertEqual({case["workload"] for case in self.native["cases"]}, {"upload_mixed"})
        self.assertEqual(self.live["measure_s"], 60)
        self.assertEqual(self.live["response_timeout_s"], 3)
        self.assertEqual(self.live["source_fps"], 30)
        self.assertEqual(self.live["scroll_px_s"], 400)
        self.assertEqual(self.live["probe_hz"], 2)
        self.assertEqual({case["scene"] for case in self.live["cases"]}, {"text-scroll"})

    def test_live_suite_passes_production_input_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            client = Path(folder) / "candidate"
            client.write_bytes(b"validation fixture; never executed")
            lab = dict(lab_id="loss-stress-fixture", ssh_target="fixture",
                       windows_root="C:/fixture", windows_task="fixture", username="fixture",
                       password_env="LOSS_STRESS_FIXTURE_SECRET", certificate_sha256="ab" * 32,
                       rdp_arguments=[])
            validate_inputs(self.live, lab, [dict(id="candidate", client=str(client))], self.profiles)


if __name__ == "__main__":
    unittest.main()

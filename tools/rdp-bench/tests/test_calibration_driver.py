"""Pure packet/aggregation tests; deliberately no networking or performance load."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import calibrate


class CalibrationDriverTests(unittest.TestCase):
    def test_packet_round_trip_and_tamper_detection(self):
        body = calibrate.payload(11, 1232)
        datagram = calibrate.packet(calibrate.PROBE, 0, 27, 100, 200, body)
        self.assertEqual(len(datagram), 1232)
        self.assertEqual(calibrate.decode(datagram, 11), (calibrate.PROBE, 0, 27, 100, 200, body))
        damaged = bytearray(datagram)
        damaged[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC'):
            calibrate.decode(damaged, 11)
        with self.assertRaisesRegex(ValueError, 'payload'):
            calibrate.decode(datagram, 29)

    def test_directional_seed_matches_documented_link_model(self):
        from rdpbench.link import Link
        for direction, name in enumerate(('c2s', 's2c')):
            link = Link({}, 11 + (1000003 if direction else 0), name)
            for seq in range(100):
                self.assertEqual(calibrate.seeded_loss(11, direction, seq, .3),
                                 link.uniform(seq + 1, 0) < .3)

    def test_peak_sliding_window_has_half_open_boundary(self):
        self.assertEqual(calibrate.peak_pps([], .01), 0)
        self.assertEqual(calibrate.peak_pps([0, 1_000_000, 10_000_000], .01), 200)
        self.assertEqual(calibrate.peak_pps([0, 1, 2], .01), 300)

    def test_observer_comparison_preserves_bad_overhead(self):
        def sample(seed, enabled, cpu, rate):
            return {'mode': 'headroom', 'seed': seed, 'observer_enabled': enabled,
                    'cpu_total_s': cpu, 'delivered_pps': rate, 'relay_forward_p99_lateness_ms': .1}
        study = calibrate.observer_comparison([sample(11, False, 1, 5000), sample(11, True, 1.3, 4500)])
        self.assertFalse(study['pass'])
        self.assertAlmostEqual(study['median_cpu_delta_fraction'], .3)
        self.assertAlmostEqual(study['median_delivered_rate_delta_fraction'], -.1)


if __name__ == '__main__':
    unittest.main()

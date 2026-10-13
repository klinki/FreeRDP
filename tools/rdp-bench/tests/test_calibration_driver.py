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

    def test_high_loss_receiver_envelope_uses_loss_draw_cohort(self):
        for loss in (0, .25, .5, .7):
            with self.subTest(loss=loss):
                bounds = calibrate.receiver_rate_bounds(9, 10, loss, 30000, 30, 1260)
                self.assertTrue(bounds['valid'])
                self.assertAlmostEqual(bounds['expected_wire_mbps'], 9 * (1 - loss))
                self.assertEqual(bounds['loss_draw_candidates'], 30000)
                expected = 9 * (1 - loss)
                self.assertLessEqual(bounds['lower_wire_mbps'], expected)
                self.assertGreaterEqual(bounds['upper_wire_mbps'], expected)
                if loss:
                    self.assertLess(bounds['upper_wire_mbps'], 9)
        # Saturation caps service at 10; admitted packets, including drops,
        # determine uncertainty rather than the much smaller surviving cohort.
        saturated = calibrate.receiver_rate_bounds(20, 10, .7, 30000, 30, 1260)
        self.assertAlmostEqual(saturated['expected_wire_mbps'], 3)
        fewer = calibrate.receiver_rate_bounds(20, 10, .7, 300, 30, 1260)
        self.assertGreater(fewer['finite_cohort_probability_delta'],
                           saturated['finite_cohort_probability_delta'])
        self.assertGreater(fewer['upper_wire_mbps'], saturated['upper_wire_mbps'])

    def test_receiver_envelope_preserves_actual_offer_and_unknown_cohort(self):
        bounds = calibrate.receiver_rate_bounds(8.8, 10, .5, 10000, 30, 1260)
        self.assertAlmostEqual(bounds['expected_wire_mbps'], 4.4)
        self.assertFalse(calibrate.receiver_rate_bounds(9, 10, .5, 0, 30, 1260)['valid'])
        self.assertFalse(calibrate.receiver_rate_bounds(9, 10, .5, True, 30, 1260)['valid'])
        self.assertFalse(calibrate.receiver_rate_bounds(float('nan'), 10, .5, 100, 30, 1260)['valid'])
        total = calibrate.receiver_rate_bounds(9, 10, 1, 10000, 30, 1260)
        self.assertEqual(total['expected_wire_mbps'], 0)
        self.assertEqual(total['finite_cohort_probability_delta'], 0)

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

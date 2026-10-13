"""Protect conservative pairing when queue trials fail or change configuration."""
import unittest

from queue_report import pair


class QueueReportTests(unittest.TestCase):
    metric = 'c2s.verified_goodput_mbps'

    def row(self, variant, *, key='same', complete=True):
        return {'trial_id': variant, 'variant': variant, 'case_id': 'upload',
                'comparison_key': key, 'seed': 11, 'validity': 'PROVISIONAL_UNCALIBRATED',
                'sut_outcome': 'COMPLETE' if complete else 'FAIL',
                'artifact_intact_timing_valid_complete': complete,
                'validated_lab_complete': False, 'metrics': {self.metric: 4.0}}

    def test_changed_configuration_is_unpaired(self):
        result, unpaired = pair([self.row('q32'), self.row('q48', key='changed')],
                                'q32', 'q48', [self.metric])
        self.assertEqual(result, [])
        self.assertEqual(len(unpaired), 2)

    def test_failed_service_remains_diagnostic(self):
        result, unpaired = pair([self.row('q32'), self.row('q48', complete=False)],
                                'q32', 'q48', [self.metric])
        self.assertEqual(unpaired, [])
        subsets = {row['subset']: row for row in result}
        self.assertEqual(len(subsets['all_observed_diagnostic']['pairs']), 1)
        valid = subsets['artifact_intact_timing_valid_complete']
        self.assertEqual(valid['pairs'], [])
        self.assertEqual(len(valid['excluded_pairs']), 1)

    def test_duplicate_seed_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'duplicate variant/configuration/seed'):
            pair([self.row('q32'), self.row('q32'), self.row('q48')],
                 'q32', 'q48', [self.metric])


if __name__ == '__main__':
    unittest.main()

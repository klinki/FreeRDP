"""Keep enabled coverage tied to actual two-mode qualification evidence."""
from dataclasses import asdict
import json
from pathlib import Path
import unittest

from profiles import PIN, options_for, profile_cases, suite_patches


class QualificationTests(unittest.TestCase):
    def test_every_enabled_case_has_passing_evidence_for_its_actual_options(self):
        record = json.loads((Path(__file__).parent/'validation-batches.json').read_text())
        selected = profile_cases('extended')
        self.assertEqual(set(record['qualification']['passed_cases_in_both_modes']), set(selected))
        self.assertEqual(record['profiles']['extended_cases'], len(selected))
        self.assertEqual(record['microsoft']['patches'], suite_patches())
        latest = {}
        for batch in record['qualification']['batches']:
            for result in batch['results']:
                latest[(result['case'], result['mode'])] = result
        for case in selected:
            for mode in ('standard', 'bridge'):
                with self.subTest(case=case, mode=mode):
                    result = latest[(case, mode)]
                    self.assertTrue(result['passed'])
                    self.assertEqual(result['errors'], [])
                    self.assertEqual(result['client_launches'], 1)
                    self.assertEqual(result['options'], asdict(options_for(case)))
                    if options_for(case).auto_reconnect:
                        self.assertEqual((result['reconnects'], result['relay_connections']), (1, 2))
                    if options_for(case).graceful_close:
                        self.assertEqual(result['graceful_closes'], 1)
        self.assertEqual(PIN['profiles']['quick']['cases'], selected[:6])


if __name__ == '__main__':
    unittest.main()

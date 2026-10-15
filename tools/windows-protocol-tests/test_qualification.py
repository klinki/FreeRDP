"""Keep enabled coverage tied to actual two-mode qualification evidence."""
from collections import Counter
from dataclasses import asdict
from datetime import datetime
import json
from pathlib import Path
import unittest

from profiles import PIN, options_for, profile_cases, suite_patches


class QualificationTests(unittest.TestCase):
    def test_every_enabled_case_has_passing_evidence_for_its_actual_options(self):
        root = Path(__file__).parent
        config = json.loads((root/'catalog-config.json').read_text())
        record = json.loads((root/config['qualification']['record']).read_text())
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


    def test_parallel_qualification_covers_every_worker_and_real_listener_overlap(self):
        root = Path(__file__).parent
        config = json.loads((root/'catalog-config.json').read_text())
        record = json.loads((root/config['qualification']['record']).read_text())
        parallel = record.get('parallel')
        if parallel is None:
            self.skipTest('Historical qualification predates parallel validation')
        batches = {batch['id']: batch for batch in record['qualification']['batches']}
        workers = parallel['workers']
        self.assertEqual(len(workers), 4)
        self.assertEqual(len(set(workers)), 4)
        selected = profile_cases('extended')
        modes = []
        for worker in workers:
            batch = batches[worker]
            results = batch['results']
            self.assertEqual([result['case'] for result in results], selected)
            mode = results[0]['mode']
            modes.append(mode)
            self.assertEqual(len(batch['startups']), len(selected))
            self.assertEqual(batch['provenance']['microsoft']['patches'], suite_patches())
            for result, startup in zip(results, batch['startups']):
                with self.subTest(worker=worker, case=result['case']):
                    self.assertEqual(result['mode'], mode)
                    self.assertTrue(result['passed'])
                    self.assertEqual(result['errors'], [])
                    self.assertEqual(result['options'], asdict(options_for(result['case'])))
                    self.assertEqual(result['client_launches'], 1)
                    options = options_for(result['case'])
                    if options.auto_reconnect:
                        self.assertEqual((result['reconnects'], result['relay_connections']), (1, 2))
                    if options.graceful_close:
                        self.assertEqual(result['graceful_closes'], 1)
                    self.assertEqual(startup['adapter_errors'], [])
                    self.assertEqual(startup['driver_exit'], 0)
                    self.assertEqual(startup['listener'], result['listener'])
                    self.assertEqual(startup['listener']['address'], '127.0.0.1')
                    self.assertGreater(startup['listener']['port'], 0)
        self.assertEqual(Counter(modes), {'standard': 2, 'bridge': 2})
        snapshot = parallel['live_listener_snapshot']
        observed = datetime.fromisoformat(snapshot['observed_at'])
        self.assertEqual({item['run_id'] for item in snapshot['listeners']}, set(workers))
        self.assertEqual(len({item['port'] for item in snapshot['listeners']}), 4)
        self.assertEqual(len({item['pid'] for item in snapshot['listeners']}), 4)
        for item in snapshot['listeners']:
            startup = batches[item['run_id']]['startups'][item['case_index'] - 1]
            self.assertLessEqual(datetime.fromisoformat(startup['started']), observed)
            self.assertGreaterEqual(datetime.fromisoformat(startup['finished']), observed)
            for key in ('address', 'port', 'pid'):
                self.assertEqual(item[key], startup['listener'][key])


if __name__ == '__main__':
    unittest.main()

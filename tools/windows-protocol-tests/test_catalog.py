"""Catalog integrity and actual JavaScript filter checks (stdlib + Node.js).

Run beside suite.json, or set WPTS_SUITE to a manifest while staging changes.
"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('catalog_generator', HERE / 'generate-catalog.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class CatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = (HERE / 'test-catalog.html').read_text()
        cls.template = (HERE / 'catalog-template.html').read_text()
        cls.catalog = json.loads(re.search(r'const catalog = (.*);\n// Pure', cls.html)[1])
        cls.manifest = json.loads(Path(os.environ.get('WPTS_SUITE', HERE / 'suite.json')).read_text())
        cls.config = json.loads((HERE / 'catalog-config.json').read_text())

    def test_all_source_cases_have_unambiguous_membership_and_rationale(self):
        rows = self.catalog['tests']
        profiles = generator.resolve_profiles(self.manifest)
        self.assertEqual(len(rows), self.config['inventory_count'])
        self.assertEqual(len(rows), len({row['name'] for row in rows}))
        for row in rows:
            with self.subTest(name=row['name']):
                memberships = [name for name in ('quick', 'extended') if row['name'] in profiles[name]]
                self.assertEqual(row['suites'], memberships)
                self.assertEqual(row['status'], 'implemented' if memberships else 'not-implemented')
                for field in ('summary', 'description', 'reason', 'area', 'disposition'):
                    self.assertTrue(row[field].strip(), field)
                self.assertIn(self.manifest['revision'], row['source_url'])
                self.assertRegex(row['source_url'], r'#L\d+$')
                self.assertFalse(row['suites'] and row['disposition'].startswith('Inapplicable'))

    def test_extended_is_superset_and_duplicate_or_cycle_is_rejected(self):
        profiles = generator.resolve_profiles(self.manifest)
        self.assertTrue(set(profiles['quick']) <= set(profiles['extended']))
        duplicate = copy.deepcopy(self.manifest)
        duplicate['profiles']['extended']['cases'].append(profiles['quick'][0])
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            generator.resolve_profiles(duplicate)
        cycle = copy.deepcopy(self.manifest)
        cycle['profiles']['quick']['include'] = ['extended']
        with self.assertRaisesRegex(ValueError, 'cycle'):
            generator.resolve_profiles(cycle)

    def test_inventory_and_manifest_drift_are_rejected(self):
        rows = self.catalog['tests']
        with patch.object(generator, 'extract_tests', return_value=rows[:-1]):
            with self.assertRaisesRegex(ValueError, 'inventory changed'):
                generator.build_catalog(Path('.'), self.manifest, self.config)
        unknown = copy.deepcopy(self.manifest)
        unknown['profiles']['extended']['cases'].append('NotAMicrosoftTest')
        with patch.object(generator, 'extract_tests', return_value=rows):
            with self.assertRaisesRegex(ValueError, 'absent from the source'):
                generator.build_catalog(Path('.'), unknown, self.config)
        stale = copy.deepcopy(self.config)
        stale['overrides']['NoLongerExists'] = {'reason': 'stale'}
        with patch.object(generator, 'extract_tests', return_value=rows):
            with self.assertRaisesRegex(ValueError, 'Stale classification'):
                generator.build_catalog(Path('.'), self.manifest, stale)

    def test_revision_change_requires_review(self):
        changed = dict(self.manifest, revision='0' * 40)
        with self.assertRaisesRegex(ValueError, 'revision changed'):
            generator.build_catalog(Path('.'), changed, self.config)

    def test_source_data_cannot_terminate_inline_script(self):
        rendered = generator.render({'text': '</script><script>alert(1)</script>'}, self.template)
        self.assertNotIn('</script><script>alert', rendered)
        self.assertIn('\\u003c/script>', rendered)

    def test_html_is_current_for_embedded_data_and_offline(self):
        self.assertEqual(generator.render(self.catalog, self.template), self.html)
        self.assertNotRegex(self.html, r'<(?:script|link|img)\b[^>]*(?:src|href)=["\']https?://')
        self.assertIn('Copyright (c) Microsoft Corporation', self.catalog['license'])

    @unittest.skipUnless(shutil.which('node'), 'Node.js is required to execute the actual UI filter function')
    def test_actual_javascript_filters_combine_correctly(self):
        function = self.template.split('function matches(test, filters) {', 1)[1].split('\nfunction el(', 1)[0]
        script = '''const fs = require('fs');
const catalog = JSON.parse(fs.readFileSync(0, 'utf8'));
function matches(test, filters) {''' + function + '''
const assert = require('assert/strict');
const all = {area:'all', suite:'all', status:'all', query:''};
const select = patch => catalog.tests.filter(t => matches(t, {...all, ...patch}));
assert.equal(select({}).length, catalog.counts.all);
assert.equal(select({suite:'quick'}).length, catalog.counts.quick);
assert.equal(select({suite:'extended'}).length, catalog.counts.extended);
assert.equal(select({suite:'extended-only'}).length, catalog.counts.extended-catalog.counts.quick);
assert.equal(select({suite:'unassigned'}).length, catalog.counts['not-implemented']);
assert.equal(select({status:'not-implemented'}).length, catalog.counts['not-implemented']);
assert.equal(select({suite:'quick',status:'not-implemented'}).length, 0);
assert.equal(select({query:'no-such-test-8732'}).length, 0);
const sample = catalog.tests.find(t=>t.suites.includes('quick'));
assert.deepEqual(select({area:sample.area,suite:'extended',status:'implemented',query:sample.name.toUpperCase()}).map(t=>t.name),[sample.name]);
assert(select({query:'inapplicable'}).length > 0);
assert.equal(select({}).length,catalog.counts.all); // default filter values; DOM reset wiring is checked separately in browser QA
console.log('UI matching: suite containment, combined selects, search and empty state passed');
'''
        result = subprocess.run(['node', '-e', script], input=json.dumps(self.catalog), text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()

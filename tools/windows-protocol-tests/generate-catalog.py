#!/usr/bin/env python3
"""Build the offline RDP Client test catalog from a pinned Microsoft Git tree.

Uses only the Python standard library and git. Never downloads source or executes
upstream code. Reads Git objects at suite.json's revision, ignoring build edits.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE_ROOT = 'TestSuites/RDP/Client/src/TestSuite/'
STRING = r'@"(?:[^"]|"")*"|"(?:[^"\\]|\\.)*"'


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args], text=True)


def decode_string(value):
    if value.startswith('@'):
        return value[2:-1].replace('""', '"')
    return json.loads(value)


def attribute(text, name):
    pattern = rf'\[{name}\(\s*((?:{STRING})(?:\s*\+\s*(?:{STRING}))*)\s*\)\]'
    return [''.join(decode_string(s) for s in re.findall(STRING, m))
            for m in re.findall(pattern, text, re.S)]


def extract_tests(source, revision):
    files = git(source, 'ls-tree', '-r', '--name-only', revision, '--', SOURCE_ROOT).splitlines()
    rows = []
    for path in sorted(p for p in files if p.endswith('.cs')):
        content = git(source, 'show', f'{revision}:{path}')
        starts = list(re.finditer(r'^\s*\[TestMethod\]', content, re.M))
        for index, start in enumerate(starts):
            end = starts[index + 1].start() if index + 1 < len(starts) else len(content)
            block = content[start.start():end]
            method = re.search(r'public\s+void\s+(\w+)\s*\(\s*\)', block)
            if not method:
                raise ValueError(f'Unrecognized TestMethod declaration: {path}')
            attrs = block[:method.start()]
            descriptions = attribute(attrs, 'Description')
            if len(descriptions) != 1:
                raise ValueError(f'Expected one description for {method[1]}')
            # The pinned tree has one public test method per attribute block.
            # Keep only this method's body for directly visible prerequisites.
            body = block[method.end():]
            body = body.split('\n        [', 1)[0]
            commands = sorted(set(re.findall(r'\.((?:Trigger)\w+)\s*\(', body)))
            requirements = []
            for line in body.splitlines():
                if '.Assume.' in line or 'Assert.IsTrue(isClientSupport' in line:
                    literals = re.findall(STRING, line)
                    if literals:
                        requirements.append(decode_string(literals[0]))
            rows.append({
                'name': method[1], 'file': path[len(SOURCE_ROOT):],
                'line': content.count('\n', 0, start.start() + method.start()) + 1,
                'description': ' '.join(descriptions[0].split()),
                'categories': attribute(attrs, 'TestCategory'),
                'commands': commands, 'requirements': list(dict.fromkeys(requirements)),
            })
    names = [row['name'] for row in rows]
    if len(names) != len(set(names)):
        raise ValueError('Duplicate test names in source; case names are no longer unique')
    return rows


def resolve_profiles(manifest):
    profiles = manifest['profiles']
    if set(profiles) != {'quick', 'extended'}:
        raise ValueError('Catalog requires explicit review for new profile names')
    def resolve(name, stack=()):
        if name in stack:
            raise ValueError('Profile include cycle')
        result = []
        for parent in profiles[name].get('include', []):
            result.extend(resolve(parent, stack + (name,)))
        result.extend(profiles[name].get('cases', []))
        if len(result) != len(set(result)):
            raise ValueError(f'Duplicate test in profile {name}')
        return result
    result = {name: resolve(name) for name in profiles}
    if not set(result['quick']) <= set(result['extended']):
        raise ValueError('Extended must include all Quick tests')
    return result


def brief(description):
    text = re.sub(r'^(?:This test case is used to (?:test|verify|ensure|check if)|This test case is to verify|This test is used to test|This test case verifies that|This test case ensures|This test case tests:?|This test verify|Verify(?: that)?|Test(?: that)?)\s+', '', description, flags=re.I)
    text = re.sub(r'^SUT\b', 'Client', text)
    text = re.sub(r'^the RDP client\b', 'Client', text)
    text = re.sub(r'^1[.)]\s*', '', text)
    text = text[0].upper() + text[1:]
    # Keep short descriptions as readable excerpts; the complete upstream
    # Description attribute remains available in each row's Source details.
    if len(text) > 185:
        text = text[:182].rsplit(' ', 1)[0].rstrip(' ,;:') + '…'
    return text


def build_catalog(source, manifest, config):
    revision = manifest['revision']
    if revision != config['revision']:
        raise ValueError('Pinned revision changed: review classifications and inventory')
    rows = extract_tests(source, revision)
    names = {r['name'] for r in rows}
    digest = hashlib.sha256(('\n'.join(sorted(names)) + '\n').encode()).hexdigest()
    if len(rows) != config['inventory_count'] or digest != config['inventory_names_sha256']:
        raise ValueError('Pinned test inventory changed: review missing/new methods')
    profiles = resolve_profiles(manifest)
    if not set(profiles['extended']) <= names:
        raise ValueError('Manifest references tests absent from the source catalog')
    if not set(manifest.get('case_options', {})) <= set(profiles['extended']):
        raise ValueError('Case options exist for unassigned tests')
    if not set(config['overrides']) <= names:
        raise ValueError('Stale classification override references absent test')
    base = manifest['repository'].removesuffix('.git') + '/blob/' + revision + '/' + SOURCE_ROOT
    for row in rows:
        matching = [rule for rule in config['groups'] if re.search(rule['file_pattern'], row['file'])]
        if len(matching) != 1:
            raise ValueError(f'Expected exactly one area group for {row["file"]}')
        group = matching[0]
        override = config['overrides'].get(row['name'], {})
        row['area'] = override.get('area', group['area'])
        row['protocol'] = row['file'].split('/')[0]
        row['summary'] = override.get('summary', brief(row['description']))
        row['suites'] = [p for p in ('quick', 'extended') if row['name'] in profiles[p]]
        row['status'] = 'implemented' if row['suites'] else 'not-implemented'
        row['disposition'] = 'Integrated' if row['suites'] else override.get('disposition', group['disposition'])
        row['reason'] = (override.get('implemented_note', 'Integrated into the runner manifest. See the qualification record for validation evidence.')
                         if row['suites'] else override.get('reason', group['reason']))
        if not row['reason'].strip():
            raise ValueError(f'Missing rationale for {row["name"]}')
        row['options'] = manifest.get('case_options', {}).get(row['name'], {})
        row['source_url'] = base + row['file'] + '#L' + str(row['line'])
        row['evidence'] = override.get('evidence', [])
    rows.sort(key=lambda r: (r['area'], r['name']))
    license_text = git(source, 'show', f'{revision}:LICENSE.txt')
    return {'revision': revision, 'review_date': config['review_date'], 'tests': rows,
            'inventory_hash': digest, 'manifest_hash': hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest(),
            'counts': {'all': len(rows), 'quick': len(profiles['quick']), 'extended': len(profiles['extended']),
                       'implemented': len(profiles['extended']), 'not-implemented': len(rows) - len(profiles['extended'])},
            'qualification': config['qualification'], 'license': license_text}


def render(catalog, template):
    data = json.dumps(catalog, ensure_ascii=False, separators=(',', ':')).replace('<', '\\u003c')
    return template.replace('/* CATALOG_DATA */ null', data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Existing Microsoft Git checkout containing the pinned revision')
    parser.add_argument('--suite', type=Path, default=HERE / 'suite.json')
    parser.add_argument('--config', type=Path, default=HERE / 'catalog-config.json')
    parser.add_argument('--output', type=Path, default=HERE / 'test-catalog.html')
    parser.add_argument('--check', action='store_true', help='Fail if the checked-in HTML differs from regenerated output')
    args = parser.parse_args(argv)
    try:
        catalog = build_catalog(args.source, json.loads(args.suite.read_text()), json.loads(args.config.read_text()))
        output = render(catalog, (HERE / 'catalog-template.html').read_text())
        if args.check:
            if not args.output.exists() or args.output.read_text() != output:
                raise ValueError('Generated catalog is stale; regenerate it')
        else:
            args.output.write_text(output)
        print(json.dumps(catalog['counts'], sort_keys=True))
    except (ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Catalog error: {error}\n')


if __name__ == '__main__':
    main()

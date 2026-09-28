#!/usr/bin/env python3
"""Compare UDP receiver revisions using identical pre-wrap office-capture packets.

macOS/Unix Makefiles only; source snapshots, commands, and results are retained.
Runs versions sequentially in alternating order, excluding one warmup per run.
"""
import argparse
import json
from pathlib import Path
import shlex
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--packets', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--before', default='bcc863bf0')
    parser.add_argument('--after', default='63e07afe5')
    parser.add_argument('--rounds', type=int, default=8)
    parser.add_argument('--pairs', type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 1 or args.pairs < 1:
        parser.error('--rounds and --pairs must be positive')
    root, build, output = args.source.resolve(), args.build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    flags = dict(line.split(' = ', 1) for line in
                 (build / 'libfreerdp/CMakeFiles/freerdp.dir/flags.make').read_text().splitlines()
                 if ' = ' in line)
    cache = dict(line.split('=', 1) for line in (build / 'CMakeCache.txt').read_text().splitlines()
                 if '=' in line and not line.startswith(('#', '//')))
    commands = []
    for label, ref in [('before', args.before), ('after', args.after)]:
        source = output / (label + '.c')
        source.write_bytes(subprocess.check_output(['git', '-C', str(root), 'show',
                                                   ref + ':libfreerdp/core/rdpeudp.c']))
        command = ['cc', '-O3', '-DNDEBUG', '-std=gnu11', '-Wl,-dead_strip',
                   '-I' + str(root / 'libfreerdp/core'),
                   '-DREVIEW_RDPEUDP_SOURCE=' + json.dumps(str(source))]
        command += shlex.split(flags['C_DEFINES']) + shlex.split(flags['C_INCLUDES'])
        command += [str(Path(__file__).with_name('udp_receive_bench.c')),
                    '-o', str(output / label)]
        for directory in ['libfreerdp', 'winpr/libwinpr', 'client/common']:
            command += ['-L' + str(build / directory), '-Wl,-rpath,' + str(build / directory)]
        command += ['-lfreerdp3', '-lwinpr3', '-lfreerdp-client3']
        command += [cache[k] for k in ['OPENSSL_SSL_LIBRARY:FILEPATH',
                                      'OPENSSL_CRYPTO_LIBRARY:FILEPATH']]
        commands.append(command)
        result = subprocess.run(command, capture_output=True, text=True)
        (output / (label + '-build.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
    packets = [str(args.packets.resolve() / f'flow-{n}.packets') for n in (0, 2, 4, 6, 8, 10)]
    runs = []
    for pair in range(args.pairs):
        for label in (['before', 'after'] if pair % 2 == 0 else ['after', 'before']):
            command = [str(output / label), str(args.rounds), *packets]
            commands.append(command)
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            (output / f'{pair}-{label}.log').write_text(result.stdout + result.stderr)
            run = json.loads(result.stdout) | {'pair': pair, 'version': label}
            if runs and any(run[key] != runs[0][key] for key in
                            ('packets_per_round', 'bytes_per_round', 'rounds')):
                raise RuntimeError('Before/after workloads differ')
            runs.append(run)
            print(json.dumps(run), flush=True)
    summary = {}
    for label in ('before', 'after'):
        values = [r for r in runs if r['version'] == label]
        summary[label] = {key: statistics.median(r[key] for r in values) for key in
                          ('cpu_ns_per_packet', 'wall_ns_per_packet', 'transport_bytes')}
    summary['cpu_change_percent'] = 100 * (summary['after']['cpu_ns_per_packet'] /
                                         summary['before']['cpu_ns_per_packet'] - 1)
    summary['cpu_added_ns_per_packet'] = (summary['after']['cpu_ns_per_packet'] -
                                          summary['before']['cpu_ns_per_packet'])
    (output / 'results.json').write_text(json.dumps({'runs': runs, 'summary': summary}, indent=2) + '\n')
    (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

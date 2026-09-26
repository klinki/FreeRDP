#!/usr/bin/env python3
"""Build and retain the review reproductions against an existing macOS Makefile build.

Correctness cases fail on regressions; timings and metrics analysis remain
informational. Nothing in the source/build checkout is changed.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def flags(path):
    return dict(line.split(' = ', 1) for line in path.read_text().splitlines()
                if ' = ' in line)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).parent / 'results')
    args = parser.parse_args()
    root, build, output = (p.resolve() for p in (args.source, args.build, args.output))
    output.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    summary = []

    def run(name, command, cwd=None, env=None):
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                                text=True, timeout=60)
        text = result.stdout + result.stderr
        (output / f'{name}.log').write_text(text)
        summary.append({'name': name, 'command': list(map(str, command)),
                        'returncode': result.returncode})
        if result.returncode:
            raise RuntimeError(f'{name}: exit {result.returncode}; see {output / (name + ".log")}')
        return text

    run('sdl-tests', [sys.executable, '-B', str(here / 'verify_sdl.py'),
        '--source', str(root), '--build', str(build), '--output', str(output)])
    print((output / 'sdl-tests.log').read_text(), end='')
    core = flags(build / 'libfreerdp/CMakeFiles/freerdp.dir/flags.make')
    codec = flags(build / 'libfreerdp/codec/CMakeFiles/freerdp-codecs.dir/flags.make')
    links = []
    for directory in ('libfreerdp', 'winpr/libwinpr', 'client/common'):
        links += ['-L' + str(build / directory), '-Wl,-rpath,' + str(build / directory)]
    links += ['-lfreerdp3', '-lwinpr3', '-lfreerdp-client3']
    cache = dict(line.split('=', 1) for line in (build / 'CMakeCache.txt').read_text().splitlines()
                 if '=' in line and not line.startswith(('#', '//')))
    crypto = [cache[key] for key in ('OPENSSL_SSL_LIBRARY:FILEPATH',
                                     'OPENSSL_CRYPTO_LIBRARY:FILEPATH') if key in cache]
    for name in ('udp_cases', 'send_cases', 'yuv_bench', 'yuv_wait_cases'):
        config = codec if name == 'yuv_wait_cases' else core
        command = ['cc', '-std=gnu11', '-O2', '-Wl,-dead_strip', '-I' + str(root)]
        command += shlex.split(config['C_DEFINES']) + shlex.split(config['C_INCLUDES'])
        command += [str(here / (name + '.c')), '-o', str(output / name), *links, *crypto]
        run(name + '-build', command)
        print(name + ':\n' + run(name, [str(output / name)]).strip())

    sdl_dir = build / 'client/SDL/SDL3'
    config = flags(sdl_dir / 'CMakeFiles/TestSDLRenderWindow.dir/flags.make')
    for name in ('overlay_case', 'probe_case'):
        command = ['c++', '-std=c++17', '-O1', '-I' + str(root)]
        command += shlex.split(config['CXX_DEFINES']) + shlex.split(config['CXX_INCLUDES'])
        command += ['-c', str(here / (name + '.cpp')), '-o', str(output / (name + '.o'))]
        run(name + '-compile', command, cwd=sdl_dir)
        command = shlex.split((sdl_dir / 'CMakeFiles/TestSDLRenderWindow.dir/link.txt').read_text())
        for i, token in enumerate(command):
            if token.endswith('.o'):
                source = token.split('.dir/', 1)[1][:-2]
                command[i] = str(output / (name + '.o' if source.startswith('test/') else
                    'TestSDLRenderWindow-' + Path(source).name + '.o'))
        command[command.index('-o') + 1] = str(output / name)
        run(name + '-build', command, cwd=sdl_dir)
        print(name + ':\n' + run(name, [str(output / name)]).strip())

    run('skip_case-build', ['c++', '-std=c++17', '-I' + str(root),
        str(here / 'skip_case.cpp'), '-o', str(output / 'skip_case')])
    metrics = output / 'skip_case.jsonl'
    metrics.unlink(missing_ok=True)
    run('skip_case', [str(output / 'skip_case')],
        env=os.environ | {'FREERDP_SDL_RENDER_METRICS': str(metrics)})
    print('skip_case records:', len(metrics.read_text().splitlines()))
    print(run('skip_analysis', [sys.executable, '-B', str(root / 'tools/render-benchmark/analyze.py'),
        str(metrics), '--warmup-intervals', '2']))
    print(run('rotation_case', [sys.executable, '-B', str(here / 'log_rotation_case.py'),
        '--source', str(root / 'free-rdp-office-debug.py'), '--output', str(output)]))
    (output / 'commands.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('Logs and binaries retained in', output)


if __name__ == '__main__':
    main()

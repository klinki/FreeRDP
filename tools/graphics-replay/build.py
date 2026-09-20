#!/usr/bin/env python3
"""Link an offline runner with an existing Unix-Makefiles SDL3 client build.

Does not rebuild or replace the client. A generated header overlay adds only
friend declarations for offline setup/readback; all renderer objects are the
existing production objects, including SdlContext's per-monitor dispatch.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    source, build, output = args.source.resolve(), args.build.resolve(), args.output.resolve()
    directory = build / 'client/SDL/SDL3'
    generated = directory / 'CMakeFiles/sdl3-freerdp.dir'
    output.parent.mkdir(parents=True, exist_ok=True)
    overlay = output.parent / (output.name + '-headers')
    overlay.mkdir(exist_ok=True)
    for name, declaration in [('sdl_context.hpp', 'class SdlContext'), ('sdl_window.hpp', 'class SdlWindow')]:
        data = (source / 'client/SDL/SDL3' / name).read_text()
        marker = declaration + '\n{'
        if data.count(marker) != 1:
            raise RuntimeError(f'Cannot add diagnostic friend to {name}')
        (overlay / name).write_text(data.replace(marker, marker + '\n\tfriend class SdlGraphicsReplay;', 1))
    flags = {}
    for line in (generated / 'flags.make').read_text().splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            flags[key] = shlex.split(value)
    link = shlex.split((generated / 'link.txt').read_text())
    compiler = link[0]
    obj = output.with_suffix('.o')
    command = [compiler, '-I' + str(overlay), '-I' + str(source / 'client/SDL/SDL3')]
    for key in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
        command += flags[key]
    command += ['-c', str(Path(__file__).with_name('replay.cpp').resolve()), '-o', str(obj)]
    subprocess.run(command, cwd=directory, check=True)
    main_objects = [x for x in link if x.endswith('sdl_freerdp.cpp.o')]
    if len(main_objects) != 1:
        raise RuntimeError('Expected exactly one SDL main object')
    link = [str(obj) if x == main_objects[0] else x for x in link]
    cache_objects = sorted((build / 'libfreerdp/CMakeFiles/freerdp.dir/cache').glob('*.c.o'))
    cache_objects = [x for x in cache_objects if x.name != 'persistent.c.o']
    if len(cache_objects) != 8:
        raise RuntimeError('Expected eight production cache objects for offline GDI setup')
    link[1:1] = [str(x) for x in cache_objects] + [
        str(build / 'libfreerdp/CMakeFiles/freerdp.dir/core/graphics.c.o')]
    link[link.index('-o') + 1] = str(output)
    subprocess.run(link, cwd=directory, check=True)
    production_objects = [directory / x for x in link if x.endswith('.o') and x != str(obj)]
    manifest = {'source': str(source), 'build': str(build), 'binary': str(output),
                'source_revision': subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip(),
                'source_revision_note': 'Checkout revision, not necessarily the build revision; object and library hashes identify the replayed code.',
                'harness_sha256': hashlib.sha256(Path(__file__).with_name('replay.cpp').read_bytes()).hexdigest(),
                'binary_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'production_objects': {str(x.resolve()): hashlib.sha256(x.read_bytes()).hexdigest() for x in production_objects},
                'libraries': {str((directory / x).resolve()): hashlib.sha256((directory / x).read_bytes()).hexdigest()
                              for x in link if x.endswith(('.dylib', '.a', '.so'))}}
    output.with_suffix('.build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Create only this owned Linux runner and build two frozen production clients."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import time
import uuid

ROOT = Path('/Users/david/projects/FreeRDP')
BASE = Path(__file__).resolve().parent
NAME = 'freerdp-pr-vm-20261014'
IMAGE = 'debian:bookworm-slim'
OWNER = 'freerdp-vm-comparison:' + uuid.uuid4().hex
LABEL = 'org.freerdp.benchmark.owner'
SOURCES = {
    'local': (BASE / 'local-source', '/src-local', '79ee244a4d5b2d5a280e165be4c643478484bf9e'),
    'pr': (ROOT / 'build/pr13608-reference-20261013/original-source', '/src-pr', 'a306a67c198f4493d8d2000bc10efd75b2432020'),
}
PACKAGES = [
    'build-essential', 'ca-certificates', 'cmake', 'ninja-build', 'pkg-config', 'git',
    'python3', 'iproute2', 'iputils-ping', 'kmod', 'ethtool', 'tcpdump', 'procps',
    'xvfb', 'xauth', 'fonts-dejavu-core', 'libssl-dev', 'zlib1g-dev',
    'libx11-dev', 'libxext-dev', 'libxi-dev', 'libxinerama-dev', 'libxrandr-dev',
    'libxrender-dev', 'libxcursor-dev', 'libxfixes-dev', 'libxv-dev',
    'libxdamage-dev', 'libxkbfile-dev', 'libasound2-dev', 'libusb-1.0-0-dev',
    'liburiparser-dev', 'libjansson-dev', 'libjson-c-dev', 'libicu-dev',
    'libswscale-dev', 'libavutil-dev', 'libavcodec-dev', 'libswresample-dev',
]
COMMON = [
    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON',
    '-DBUILD_TESTING=OFF', '-DWITH_CLIENT=ON', '-DWITH_X11=ON',
    '-DWITH_CLIENT_SDL=OFF', '-DWITH_WAYLAND=OFF', '-DWITH_SERVER=OFF',
    '-DWITH_SAMPLE=OFF', '-DWITH_OPENSSL=ON', '-DWITH_FFMPEG=OFF',
    '-DWITH_OPENH264=OFF', '-DWITH_OPUS=OFF', '-DWITH_GFX_AV1=OFF',
    '-DWITH_AOM=OFF', '-DWITH_DAV1D=OFF', '-DWITH_YUV=OFF', '-DWITH_JPEG=OFF',
    '-DWITH_CUPS=OFF', '-DWITH_PCSC=OFF', '-DWITH_KRB5=OFF', '-DWITH_PULSE=OFF',
    '-DWITH_MANPAGES=OFF', '-DWITH_DOCUMENTATION=OFF', '-DWITH_CCACHE=OFF',
    '-DWITH_DEBUG_ALL=OFF', '-DWITH_SWSCALE=ON', '-DWITH_FUSE=OFF',
]

def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')

def docker(arguments, *, logfile=None, check=True):
    command = ['docker', *arguments]
    if logfile:
        with (BASE / logfile).open('xb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
    else:
        result = subprocess.run(command, text=True, capture_output=True, check=False)
    if check and result.returncode:
        raise RuntimeError(f'command exited {result.returncode}; log={logfile}; argv={command!r}')
    return result

def source_receipt(path):
    files = {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(path.rglob('*')) if p.is_file()}
    encoded = json.dumps(files, sort_keys=True, separators=(',', ':')).encode()
    return {'files': files, 'file_count': len(files), 'tree_sha256': hashlib.sha256(encoded).hexdigest()}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--resume-configure', action='store_true')
    args = parser.parse_args()
    resume = args.resume_configure
    if (BASE / 'linux-setup.json').exists() and not resume:
        raise RuntimeError('Linux setup receipt exists; refusing repeat or replacement')
    for path, _, _ in SOURCES.values():
        if not (path / 'CMakeLists.txt').is_file():
            raise RuntimeError('missing frozen source: ' + str(path))
    existing = docker(['container', 'inspect', NAME], check=False)
    if existing.returncode == 0 and not resume:
        raise RuntimeError('container name already exists; refusing to modify it')
    if existing.returncode != 0 and 'No such container' not in existing.stderr:
        raise RuntimeError('cannot establish unused Docker container name: ' + existing.stderr)
    image = json.loads(docker(['image', 'inspect', IMAGE]).stdout)[0]
    receipt = json.loads((BASE / 'linux-setup.json').read_text()) if resume else {
        'schema_version': 1, 'owner': OWNER, 'container_name': NAME, 'container_id': None,
        'started_unix_s': time.time(), 'image_reference': IMAGE, 'image_id': image['Id'],
        'image_repo_digests': image.get('RepoDigests'), 'image_architecture': image.get('Architecture'),
        'resources': {'cpus': 4, 'memory_bytes': 4294967296},
        'network': 'isolated Docker bridge; NET_ADMIN only; no host network',
        'packages_requested': PACKAGES, 'builds': {},
        'scope': 'production X11 clients; no private adapter; no Xvfb, VM or network trials launched',
    }
    if resume:
        current = json.loads(existing.stdout)[0]
        if (receipt.get('outcome') != 'FAILED' or current['Id'] != receipt['container_id']
                or current['Config']['Labels'].get(LABEL) != receipt['owner']
                or current['Image'] != receipt['image_id']
                or current['HostConfig']['NanoCpus'] != 4000000000
                or current['HostConfig']['Memory'] != 4294967296
                or current['HostConfig']['NetworkMode'] != 'bridge'
                or current['HostConfig']['Privileged']):
            raise RuntimeError('resume requires exact failed owned-container receipt and resources')
        backup = BASE / 'linux-setup-attempt1.json'
        if backup.exists():
            raise RuntimeError('resume receipt already exists; refusing another attempt')
        save(backup, receipt)
        receipt['previous_attempt'] = backup.name
        receipt['resume_unix_s'] = time.time()
        receipt['outcome'] = 'RUNNING'
        receipt.pop('error', None)
        receipt.pop('finished_unix_s', None)
    save(BASE / 'linux-setup.json', receipt)
    command = ['run', '-d', '--name', NAME, '--label', LABEL + '=' + OWNER,
               '--cpus', '4', '--memory', '4g', '--cap-add', 'NET_ADMIN', '--network', 'bridge',
               '--mount', f'type=bind,src={BASE},dst=/bench',
               '--mount', f'type=bind,src={ROOT / "tools/rdp-bench"},dst=/suite,readonly']
    for path, container_path, _ in SOURCES.values():
        command += ['--mount', f'type=bind,src={path},dst={container_path},readonly']
    command += [image['Id'], 'sleep', 'infinity']
    if not resume:
        receipt['container_create_argv'] = command
    try:
        if not resume:
            receipt['container_id'] = docker(command).stdout.strip()
            save(BASE / 'linux-setup.json', receipt)
            docker(['exec', NAME, 'apt-get', 'update'], logfile='linux-apt-update.log')
            docker(['exec', NAME, 'env', 'DEBIAN_FRONTEND=noninteractive', 'apt-get',
                    'install', '-y', '--no-install-recommends', *PACKAGES], logfile='linux-apt-install.log')
        actual = json.loads(docker(['container', 'inspect', NAME]).stdout)[0]
        receipt['actual_container'] = {
            'id': actual['Id'], 'image_id': actual['Image'],
            'owner': actual['Config']['Labels'].get(LABEL),
            'nano_cpus': actual['HostConfig']['NanoCpus'], 'memory_bytes': actual['HostConfig']['Memory'],
            'network_mode': actual['HostConfig']['NetworkMode'],
            'privileged': actual['HostConfig']['Privileged'], 'cap_add': actual['HostConfig']['CapAdd'],
            'mounts': actual['Mounts'],
            'trace_environment_present': [v.split('=', 1)[0] for v in actual['Config']['Env']
                                          if v.split('=', 1)[0].startswith('RDPEUDP_RECOVERY_TRACE')],
        }
        if receipt['actual_container']['trace_environment_present']:
            raise RuntimeError('trace environment unexpectedly present')
        inventory = docker(['exec', NAME, 'dpkg-query', '-W', '-f=${binary:Package}\t${Version}\n']).stdout
        (BASE / 'linux-packages.tsv').write_text(inventory)
        receipt['dependency_inventory_sha256'] = hashlib.sha256(inventory.encode()).hexdigest()
        receipt['kernel'] = docker(['exec', NAME, 'uname', '-a']).stdout.strip()
        receipt['os_release'] = docker(['exec', NAME, 'cat', '/etc/os-release']).stdout.strip()
        for engine, (source, container_source, commit) in SOURCES.items():
            source_meta = source_receipt(source)
            source_path = BASE / f'linux-{engine}-source-hashes.json'
            if source_path.exists() and json.loads(source_path.read_text()) != source_meta:
                raise RuntimeError('frozen source changed before resume')
            if not source_path.exists():
                save(source_path, source_meta)
            flags = [*COMMON, *(['-DWITH_RDP_BENCH=OFF'] if engine == 'local' else ['-DWITH_RDPEUDP=ON'])]
            suffix = '-resume1' if resume else ''
            build = {'source_commit': commit, 'source_path': str(source),
                     'source_tree_sha256': source_meta['tree_sha256'], 'cmake_flags': flags,
                     'private_abi': 'disabled by WITH_RDP_BENCH=OFF' if engine == 'local' else 'absent from clean PR source',
                     'tracing': 'all RDPEUDP_RECOVERY_TRACE* environment variables absent; WITH_DEBUG_ALL=OFF',
                     'build_directory': f'/bench/linux-{engine}', 'outcome': 'BUILDING',
                     'configure_log': f'linux-{engine}-configure{suffix}.log',
                     'build_log': f'linux-{engine}-build{suffix}.log'}
            receipt['builds'][engine] = build
            save(BASE / 'linux-setup.json', receipt)
            docker(['exec', NAME, 'cmake', '-S', container_source, '-B', f'/bench/linux-{engine}', *flags],
                   logfile=build['configure_log'])
            docker(['exec', NAME, 'cmake', '--build', f'/bench/linux-{engine}', '--target', 'xfreerdp', '-j', '4'],
                   logfile=build['build_log'])
            exe = f'/bench/linux-{engine}/client/X11/xfreerdp'
            version = docker(['exec', NAME, exe, '/version']).stdout.strip()
            config = docker(['exec', NAME, exe, '/buildconfig']).stdout.strip()
            (BASE / f'linux-{engine}-version.txt').write_text(version + '\n')
            (BASE / f'linux-{engine}-buildconfig.txt').write_text(config + '\n')
            binary = BASE / f'linux-{engine}/client/X11/xfreerdp'
            libraries = {str(p.relative_to(BASE)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted((BASE / f'linux-{engine}').rglob('*.so*')) if p.is_file()}
            build.update(outcome='COMPLETE', binary_path=exe,
                         binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                         libraries_sha256=libraries, version=version, buildconfig=config)
            symbols = docker(['exec', NAME, 'nm', '-D', f'/bench/linux-{engine}/libfreerdp/libfreerdp3.so']).stdout
            exported = [line for line in symbols.splitlines() if 'freerdp_bench_v1_' in line]
            build['private_benchmark_exports'] = exported
            if exported:
                raise RuntimeError('production library unexpectedly exports private benchmark ABI')
            save(BASE / 'linux-setup.json', receipt)
            print(engine, 'production client COMPLETE', build['binary_sha256'], flush=True)
        receipt['completed_unix_s'] = time.time()
        receipt['outcome'] = 'COMPLETE'
        save(BASE / 'linux-setup.json', receipt)
        print('Both production clients ready; container retained for root-owned VM comparison.', flush=True)
    except Exception as error:
        receipt['outcome'] = 'FAILED'
        receipt['error'] = repr(error)
        receipt['finished_unix_s'] = time.time()
        save(BASE / 'linux-setup.json', receipt)
        raise

if __name__ == '__main__':
    main()

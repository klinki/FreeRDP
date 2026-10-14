#!/usr/bin/env python3
"""Restore frozen clients into a newly owned isolated Linux lab."""
from pathlib import Path
import datetime
import hashlib
import json
import subprocess
import uuid

ROOT = Path('/Users/david/projects/FreeRDP')
BASE = Path(__file__).resolve().parent
OLD = ROOT / 'build/vm-pr-comparison-20261014'
WORKTREE = Path('/Users/david/.codex/worktrees/pr13608-flight-bench/FreeRDP')
NAME = 'freerdp-protocol-loss-20261014'
OWNER = 'freerdp-protocol-loss:' + uuid.uuid4().hex

def call(args, log=None, check=True):
    if log:
        with (BASE/log).open('xb') as stream:
            result = subprocess.run(['docker', *args], stdout=stream, stderr=subprocess.STDOUT)
    else:
        result = subprocess.run(['docker', *args], capture_output=True, text=True)
    if check and result.returncode:
        raise RuntimeError('Docker operation failed; retained log=' + str(log))
    return result

def save(name, value):
    (BASE/name).write_text(json.dumps(value, indent=2) + '\n')

def main():
    if (BASE/'runtime-setup.json').exists():
        raise RuntimeError('runtime setup already attempted')
    existing = call(['inspect', NAME], check=False)
    if existing.returncode == 0 or 'no such' not in existing.stderr.casefold():
        raise RuntimeError('cannot establish unused container identity')
    previous = json.loads((OLD/'linux-setup.json').read_text())
    image = previous['image_id']
    args = ['run','-d','--name',NAME,'--label','org.freerdp.benchmark.owner='+OWNER,
            '--cpus','4','--memory','4g','--cap-add','NET_ADMIN','--network','bridge',
            '-v',str(BASE)+':/bench:rw',
            '-v',str(WORKTREE/'tools/rdp-bench')+':/suite:ro',
            '-v',str(OLD/'local-source')+':/src-local:ro',
            '-v',str(ROOT/'build/pr13608-reference-20261013/original-source')+':/src-pr:ro',
            '-v',str(BASE/'pr-trace-source')+':/src-trace:ro',
            '-v',str(BASE/'pr-fixed-source')+':/src-fixed:ro',
            image,'sleep','infinity']
    identity = call(args).stdout.strip()
    receipt = dict(owner=OWNER,container_name=NAME,container_id=identity,image_id=image,
                   created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   resources={'cpus':4,'memory_bytes':4294967296},create_argv=args,
                   outcome='RUNNING',previous_runtime_receipt=str(OLD/'linux-setup.json'))
    save('runtime-setup.json',receipt)
    call(['exec',NAME,'apt-get','update'],log='linux-apt-update.log')
    call(['exec',NAME,'env','DEBIAN_FRONTEND=noninteractive','apt-get','install','-y','--no-install-recommends',
          *previous['packages_requested']],log='linux-apt-install.log')
    packages=call(['exec',NAME,'dpkg-query','-W','-f=${binary:Package}\t${Version}\n']).stdout
    (BASE/'linux-packages.tsv').write_text(packages)
    builds=previous['builds']
    for variant,build in builds.items():
        binary=BASE/('linux-'+variant)/'client/X11/xfreerdp'
        assert hashlib.sha256(binary.read_bytes()).hexdigest()==build['binary_sha256']
        build['runtime_reused_from']=str(OLD/('linux-'+variant))
        linkage=call(['exec',NAME,'ldd',build['binary_path']]).stdout
        if 'not found' in linkage:raise RuntimeError('missing production runtime dependency')
        (BASE/f'linux-{variant}-ldd.txt').write_text(linkage)
        version=call(['exec',NAME,build['binary_path'],'/version']).stdout.strip()
        assert version==build['version']
    receipt.update(outcome='COMPLETE',builds=builds,
                   kernel=call(['exec',NAME,'uname','-a']).stdout.strip(),
                   os_release=call(['exec',NAME,'cat','/etc/os-release']).stdout,
                   dependency_inventory_sha256=hashlib.sha256(packages.encode()).hexdigest(),
                   source='Frozen prior production binaries; new isolated runtime and current dependency inventory')
    save('runtime-setup.json',receipt)
    save('linux-setup.json',receipt)
    print('Frozen local and PR runtimes ready in owned container',identity,flush=True)

if __name__=='__main__':main()

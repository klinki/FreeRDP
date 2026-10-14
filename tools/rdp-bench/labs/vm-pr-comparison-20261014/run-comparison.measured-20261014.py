#!/usr/bin/env python3
"""Eight fresh RDP transport connections using the owned upstream ingress method.

Preparation artifact: execution is intentionally explicit. Authentication is read
once with getpass and sent only over each client's stdin. No credential argument,
environment variable, receipt, log field or password file is created.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import getpass
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import re
import signal
import subprocess
import sys
import time
from types import SimpleNamespace
import uuid
import warnings

ROOT = Path('/Users/david/projects/FreeRDP')
BASE = Path(__file__).resolve().parent
HOST_HELPER = ROOT / 'tools/rdp-bench/upstream-method/host-control.py'
NET = '/suite/upstream-method/linux-ingress.py'
RAMP = '/suite/upstream-method/run-ramp.py'
STATE = '/bench/line-state.json'
CONTAINER = 'freerdp-pr-vm-20261014'
LOG_FILTERS = 'com.freerdp.core.multitransport:DEBUG,com.freerdp.core.rdpemt:DEBUG'
READY = {'local': 'ready (async)', 'pr': 'UDP multitransport tunnel ready'}


def utc():
    return datetime.now(timezone.utc).isoformat()


def save(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def container_path(path):
    # The caller must mount this exact directory as /bench in the owned container.
    return '/bench/' + str(Path(path).resolve().relative_to(BASE)).replace(os.sep, '/')


def docker(arguments, *, timeout=40, capture=True, check=True):
    result = subprocess.run(['docker', 'exec', CONTAINER, *arguments],
                            capture_output=capture, text=True, timeout=timeout, check=False)
    if check and result.returncode:
        raise RuntimeError('owned container command failed: ' + (result.stderr or result.stdout or '')[-2000:])
    return result


def host_control():
    spec = importlib.util.spec_from_file_location('vm_comparison_host_control', HOST_HELPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def checked_settings(path):
    settings = json.loads(Path(path).read_text())
    if not isinstance(settings, dict):
        raise ValueError('settings must be a nonsecret JSON object')
    forbidden = {'password', 'passwd', 'credential', 'credentials', 'secret', 'token', 'private_key'}
    def reject_secrets(value):
        if isinstance(value, dict):
            if any(str(k).casefold() in forbidden for k in value):
                raise ValueError('credential fields are forbidden in settings')
            for v in value.values():
                reject_secrets(v)
        elif isinstance(value, list):
            for v in value:
                reject_secrets(v)
    reject_secrets(settings)
    if not re.fullmatch('[0-9a-fA-F]{64}', settings.get('certificate_sha256', '')):
        raise ValueError('a pinned SHA256 certificate fingerprint is required')
    if settings.get('vm_ip') != '192.168.64.2':
        raise ValueError('this comparison is scoped to the declared Windows VM')
    if settings.get('loss_percent') != [0, 25, 50, 70]:
        raise ValueError('require the fixed 0/25/50/70 fresh-session matrix')
    if settings.get('warmup_seconds') != 45 or settings.get('measurement_seconds') != 120:
        raise ValueError('require fixed 45s warmup and 120s measurement')
    for variant in ('local', 'pr'):
        entry = settings['variants'][variant]
        expected = f'/bench/linux-{variant}/client/X11/xfreerdp'
        if entry.get('binary') != expected or not re.fullmatch('[0-9a-f]{40}', entry.get('commit', '')):
            raise ValueError('explicit expected production binary and source commit required')
    for name in ('owned-exec.py', 'measurement-driver.py'):
        if not (BASE / name).is_file():
            raise ValueError('missing reviewed local helper ' + name)
    return settings


def linux_clock():
    samples = []
    for _ in range(3):
        before = time.time()
        remote = json.loads(docker(['python3', '-c',
                                    'import json,time; print(json.dumps({"unix_s":time.time(),"monotonic_ns":time.monotonic_ns()}))']).stdout)
        after = time.time()
        samples.append(dict(host_start_unix_s=before, host_end_unix_s=after,
                            linux_unix_s=remote['unix_s'], linux_monotonic_ns=remote['monotonic_ns'],
                            offset_linux_minus_host_s=remote['unix_s'] - (before + after) / 2,
                            uncertainty_s=(after - before) / 2))
    return dict(method='host UTC bracket around owned Docker exec; shortest round trip',
                samples=samples, selected=min(samples, key=lambda row: row['uncertainty_s']))


def process_action(receipt_path, owner, sig=None):
    code = r'''import json,pathlib,os,signal,sys
r=json.loads(pathlib.Path(sys.argv[1]).read_text());owner=sys.argv[2];sig=int(sys.argv[3]);assert r['owner']==owner,'process receipt owner differs'
p=r['pid'];f=pathlib.Path(f'/proc/{p}/stat')
if not f.exists():print(json.dumps({'alive':False,'outcome':'ALREADY_EXITED','pid':p}));raise SystemExit(0)
a=f.read_text().rsplit(')',1)[1].split();assert int(a[19])==r['start_ticks'],'PID birth time differs'
if a[0] in ('Z','X'):print(json.dumps({'alive':False,'outcome':'ALREADY_EXITED','pid':p}));raise SystemExit(0)
exe=str(pathlib.Path(f'/proc/{p}/exe').resolve());assert exe==r['executable'],'process executable differs'
if sig:os.kill(p,sig)
print(json.dumps({'alive':True,'pid':p,'start_ticks':int(a[19]),'executable':exe,'outcome':'SIGNALED_OWNED_PROCESS' if sig else 'SAME_OWNED_PROCESS','signal':sig}))'''
    return json.loads(docker(['python3', '-c', code, container_path(receipt_path), owner,
                              str(int(sig) if sig else 0)]).stdout)


def network(output, operation, loss=None, expected=None):
    identity_keys = ('owner', 'netns', 'interface', 'ingress', 'receive', 'mac')
    if expected is not None:
        current = json.loads(docker(['python3', '-c', 'import pathlib;print(pathlib.Path("/bench/line-state.json").read_text())']).stdout)
        if any(current.get(key) != expected.get(key) for key in identity_keys):
            raise ValueError('owned network identity differs from preflight')
    command = ['python3', NET, '--state', STATE, '--output', container_path(output), operation]
    if loss is not None:
        command += ['--loss', str(loss)]
    docker(command)
    receipt = json.loads(Path(output).read_text())
    if expected is not None and any(receipt['state'].get(key) != expected.get(key) for key in identity_keys):
        raise ValueError('owned network identity changed during controller action')
    return receipt


def wait_file(path, child=None, timeout=15):
    deadline = time.monotonic() + timeout
    while not Path(path).is_file():
        if child and child.poll() is not None:
            raise RuntimeError('owned process exited before writing its receipt')
        if time.monotonic() >= deadline:
            raise RuntimeError('owned receipt did not arrive within bounded wait: ' + Path(path).name)
        time.sleep(.05)
    return json.loads(Path(path).read_text())


def stop_host_process(child):
    if not child or child.poll() is not None:
        return dict(outcome='ALREADY_EXITED', exit_code=child.returncode if child else None)
    # Each host subprocess is created in its own session; signal only that owned group.
    os.killpg(child.pid, signal.SIGTERM)
    try:
        return dict(outcome='OWNED_HOST_GROUP_TERMINATED', exit_code=child.wait(timeout=10))
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        return dict(outcome='OWNED_HOST_GROUP_KILLED', exit_code=child.wait(timeout=5))


def preflight(settings, output):
    source_names = {'local': ['rdpeudp.c', 'rdpeudp.h'], 'pr': ['rdpudp.c', 'rdpudp.h']}
    code = r'''import hashlib,json,os,pathlib,shutil,sys
cfg=json.loads(sys.argv[1]);result={'kernel':os.uname().release,'variants':{},'display_socket_present':pathlib.Path('/tmp/.X11-unix/X99').is_socket(),'image_script_present':pathlib.Path('/bench/image-progress.py').is_file(),'suite_helpers':{},'tcpdump':shutil.which('tcpdump')}
for n in ['linux-ingress.py','run-ramp.py']:
 p=pathlib.Path('/suite/upstream-method')/n;assert p.is_file(),str(p);result['suite_helpers'][n]=hashlib.sha256(p.read_bytes()).hexdigest()
for key,v in cfg['variants'].items():
 binary=pathlib.Path(v['binary']);assert binary.is_file() and os.access(binary,os.X_OK),str(binary)
 src=pathlib.Path('/src-'+key)/'libfreerdp/core';names=cfg['source_names'][key]
 entry={'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'source_sha256':{},'libraries_sha256':{}}
 for name in names:
  p=src/name;assert p.is_file(),str(p);entry['source_sha256'][name]=hashlib.sha256(p.read_bytes()).hexdigest()
 build=pathlib.Path('/bench/linux-'+key)
 for pattern in ['libfreerdp3.so.*','libfreerdp-client3.so.*','libwinpr3.so.*']:
  paths=sorted(build.rglob(pattern));assert paths,pattern
  for p in paths:entry['libraries_sha256'][str(p.relative_to(build))]=hashlib.sha256(p.read_bytes()).hexdigest()
 result['variants'][key]=entry
assert result['display_socket_present'],'owned Xvfb socket missing'
assert result['image_script_present'],'image observer script missing'
print(json.dumps(result))'''
    payload = dict(variants=settings['variants'], source_names=source_names)
    receipt = json.loads(docker(['python3', '-c', code, json.dumps(payload)], timeout=90).stdout)
    receipt['host_helpers_sha256'] = {str(path.relative_to(ROOT)): digest(path)
                                       for path in (Path(__file__).resolve(), BASE / 'owned-exec.py',
                                                    BASE / 'measurement-driver.py', HOST_HELPER)}
    receipt['settings_sha256'] = digest(output.parent / 'settings.json') if (output.parent / 'settings.json').is_file() else None
    receipt['utc'] = utc()
    receipt['network'] = network(output.with_name('network-preflight.json'), 'snapshot')
    save(output, receipt)
    return receipt


def run_trial(settings, h, output, trial_id, variant, loss, password, initial, *, pcap=False, save_ppm=True):
    out = output / trial_id
    out.mkdir(exist_ok=False)
    args = SimpleNamespace(ssh_target='david@192.168.64.2', ssh_socket=settings['ssh_socket'],
                           windows_root=str(PureWindowsPath(settings['windows_scripts_root']) / trial_id),
                           scripts_root=settings['windows_scripts_root'],
                           task_name='RdpBench-Pr13608-compare-' + trial_id)
    cfg = h.checked_config(args)
    ctl = h.Controller(cfg, out)
    client = collector = None
    client_log = collector_log = None
    client_receipt = None
    captures = {}
    outcome = dict(trial_id=trial_id, variant=variant, engine=variant, path=trial_id, loss_percent=loss,
                   source_commit=settings['variants'][variant]['commit'], started_utc=utc(),
                   requested_warmup_s=45, requested_measurement_s=120, outcome='FAILED_SETUP',
                   errors=[], cleanup_errors=[], host_load_before_trial=list(os.getloadavg()), scope='fresh live RDP transport connection; existing Windows login may be reused; server PDH and sampled visual progress')
    save(out / 'attempt.json', outcome)
    try:
        ctl.prepare()
        ctl.read_state()
        owner = ctl.state['owner']
        before = network(out / 'network-warmup-zero.json', 'change', 0, initial['network']['state'])
        binary = settings['variants'][variant]['binary']
        client_arguments = ['/v:' + settings['vm_ip'], '/u:' + settings['username'],
                            '/from-stdin:force', '/cert:fingerprint:sha256:' + settings['certificate_sha256'],
                            '/size:1280x800', '/bpp:32', '/gfx:progressive', '/network:auto', '/timeout:20000',
                            '/client-hostname:vmcmp-' + uuid.uuid4().hex[:8],
                            '/log-level:INFO', '/log-filters:' + LOG_FILTERS]
        if settings.get('domain'):
            client_arguments.insert(2, '/d:' + settings['domain'])
        launch = ['docker', 'exec', '-i', CONTAINER, 'env', 'DISPLAY=:99', 'WLOG_LEVEL=INFO',
                  'WLOG_FILTER=' + LOG_FILTERS, 'stdbuf', '-oL', '-eL', 'python3', '/bench/owned-exec.py',
                  '--owner', owner, '--receipt', container_path(out / 'client-owned.json'),
                  '--executable', binary, '--', *client_arguments]
        client_log = (out / 'client.log').open('xb')
        client = subprocess.Popen(launch, stdin=subprocess.PIPE, stdout=client_log,
                                  stderr=subprocess.STDOUT, start_new_session=True)
        save(out / 'client-launch.json', dict(command=launch, host_pid=client.pid, owner=owner, utc=utc()))
        client.stdin.write((password + '\n').encode())
        client.stdin.flush()
        client.stdin.close()
        client_receipt = wait_file(out / 'client-owned.json', client)
        if client_receipt['owner'] != owner:
            raise ValueError('client ownership receipt differs')
        deadline = time.monotonic() + 45
        while READY[variant] not in (out / 'client.log').read_text(errors='replace'):
            if client.poll() is not None:
                raise RuntimeError('client exited before engine-specific UDP readiness')
            if time.monotonic() >= deadline:
                raise RuntimeError('engine-specific UDP readiness timed out')
            time.sleep(.25)
        status = process_action(out / 'client-owned.json', owner)
        if not status['alive']:
            raise RuntimeError('owned client ended after UDP readiness')
        save(out / 'udp-ready.json', dict(engine=variant, required_log=READY[variant],
                                         matched=True, utc=utc(), process=status))
        discovery = ctl.discover()
        if not discovery.get('counter_instance'):
            raise RuntimeError('active RDP session has no corroborated per-instance PDH selector')
        ctl.clock()
        os.replace(out / 'clock-alignment.json', out / 'clock-before.json')
        save(out / 'linux-clock-before.json', linux_clock())
        collection_seconds = 45 + 120 + 45
        collector_command = [sys.executable, str(HOST_HELPER), '--ssh-target', cfg['ssh_target'],
                             '--ssh-socket', cfg['ssh_socket'], '--windows-root', cfg['windows_root'],
                             '--scripts-root', cfg['scripts_root'], '--task-name', cfg['task_name'],
                             '--output', str(out), 'collector', '--seconds', str(collection_seconds),
                             '--interval', '10']
        collector_log = (out / 'collector-console.log').open('xb')
        collector = subprocess.Popen(collector_command, stdout=collector_log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        save(out / 'collector-launch.json', dict(command=collector_command, host_pid=collector.pid, utc=utc()))
        load = ctl.load(collection_seconds)
        warmup_start = time.monotonic()
        print(f'{trial_id}: {variant} UDP ready; warming workload for45s', flush=True)
        while time.monotonic() - warmup_start < 45:
            if client.poll() is not None:
                raise RuntimeError('owned client exited during warmup')
            if collector.poll() is not None:
                raise RuntimeError('continuous PDH collector ended during warmup')
            time.sleep(max(0, min(1, 45 - (time.monotonic() - warmup_start))))
        provenance = dict(client_pid=client_receipt['pid'], client_start_ticks=client_receipt['start_ticks'],
                          owner=owner, variant=variant, source_commit=outcome['source_commit'],
                          binary=initial['variants'][variant], counter_instance=discovery['counter_instance'],
                          load_receipt=load, warmup=dict(minimum_requested_s=45,
                                                       observed_host_monotonic_s=time.monotonic() - warmup_start),
                          requested_loss_percent=[loss], requested_s=120,
                          readiness_log=READY[variant], client_arguments=client_arguments,
                          windows_clock_before=json.loads((out / 'clock-before.json').read_text()),
                          linux_clock_before=json.loads((out / 'linux-clock-before.json').read_text()),
                          network_identity=before['state'], collector_interval_s=10,
                          host_load_before_measurement=list(os.getloadavg()),
                          method_deviations=['owned veth replaces unavailable IFB',
                                             'small/large ICMP preflight omitted',
                                             'certificate pin and password over stdin',
                                             'INFO plus transport setup DEBUG, no per-packet UDP DEBUG'],
                          scope='upstream all-IPv4 VM ingress method; no rate cap; not verified application goodput')
        save(out / 'provenance.json', provenance)
        if pcap:
            if not initial.get('tcpdump'):
                raise RuntimeError('optional packet capture requested but tcpdump unavailable')
            for label, interface in [('offered', before['state']['interface']), ('received', before['state']['receive'])]:
                log = (out / (label + '-capture.log')).open('xb')
                receipt_path = out / (label + '-capture-owned.json')
                command = ['docker', 'exec', CONTAINER, 'python3', '/bench/owned-exec.py', '--owner', owner,
                           '--receipt', container_path(receipt_path), '--executable', initial['tcpdump'], '--',
                           '-i', interface, '-U', '-nn', '-s', '256', '-w', container_path(out / (label + '.pcap')),
                           'udp and host ' + settings['vm_ip'] + ' and port 3389']
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                captures[label] = dict(process=process, log=log, receipt=receipt_path)
                wait_file(receipt_path, process)
                capture_deadline = time.monotonic() + 10
                while 'listening on' not in (out / (label + '-capture.log')).read_text(errors='replace'):
                    if process.poll() is not None or time.monotonic() >= capture_deadline:
                        raise RuntimeError('owned packet capture did not become ready')
                    time.sleep(.05)
        command = ['docker', 'exec', CONTAINER, 'python3', '/bench/measurement-driver.py', '--ramp', RAMP,
                   '--state', STATE, '--pid', str(client_receipt['pid']), '--provenance', container_path(out / 'provenance.json'),
                   '--output', container_path(out / 'ramp'), '--owner', owner, '--loss', str(loss), '--seconds', '120',
                   '--image-script', '/bench/image-progress.py', '--owned-exec', '/bench/owned-exec.py',
                   '--display', ':99', '--region', '448', '280', '384', '240']
        if save_ppm:
            command.append('--save-ppm')
        save(out / 'measurement-launch.json', dict(command=command, utc=utc()))
        with (out / 'ramp-console.log').open('xb') as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=False)
        if completed.returncode not in (0, 1):
            raise RuntimeError('measurement wrapper failed: ' + str(completed.returncode))
        campaign = json.loads((out / 'ramp/campaign.json').read_text())
        outcome.update(outcome=campaign['steps'][0]['outcome'], measurement=campaign,
                       image_observer=json.loads((out / 'image-observer-status.json').read_text()))
        if campaign.get('error'):
            outcome['errors'].append(campaign['error'])
        if campaign.get('cleanup', {}).get('outcome') != 'LOSS_RESET_TO_ZERO':
            outcome['network_ownership_or_cleanup_failed'] = True
        print(f'{trial_id}: {outcome["outcome"]}', flush=True)
    except (Exception, KeyboardInterrupt) as error:
        outcome['errors'].append(repr(error))
        outcome['interrupted'] = isinstance(error, KeyboardInterrupt)
        save(out / 'execution-error.json', dict(error=repr(error), utc=utc()))
        print(f'{trial_id}: retained error {type(error).__name__}', flush=True)
    finally:
        if ctl.state is None and ctl.metadata.is_file():
            try:
                ctl.read_state()
            except Exception as error:
                outcome['cleanup_errors'].append('local Windows ownership receipt: ' + repr(error))
        if ctl.state:
            owner = ctl.state['owner']
            capture_health = {}
            for label, capture in captures.items():
                try:
                    if capture['process'].poll() is None:
                        process_action(capture['receipt'], owner, signal.SIGINT)
                    try:
                        code = capture['process'].wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process_action(capture['receipt'], owner, signal.SIGTERM)
                        try:
                            code = capture['process'].wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process_action(capture['receipt'], owner, signal.SIGKILL)
                            code = capture['process'].wait(timeout=5)
                    capture['log'].close()
                    text = (out / (label + '-capture.log')).read_text(errors='replace')
                    drops = re.search(r'(\d+) packets dropped by kernel', text)
                    count = re.search(r'(\d+) packets captured', text)
                    capture_health[label] = dict(exit_code=code, kernel_drops=int(drops[1]) if drops else None,
                                                captured_packets=int(count[1]) if count else None,
                                                complete=code == 0 and drops is not None,
                                                wire_evidence_valid=code == 0 and drops is not None and int(drops[1]) == 0)
                except Exception as error:
                    capture_health[label] = dict(error=repr(error))
                    outcome['cleanup_errors'].append('capture ' + label + ': ' + repr(error))
                finally:
                    capture['log'].close()
            save(out / 'capture-health.json', capture_health)
            try:
                save(out / 'linux-clock-after.json', linux_clock())
            except Exception as error:
                outcome['cleanup_errors'].append('Linux clock: ' + repr(error))
            try:
                if (out / 'clock-before.json').exists():
                    ctl.clock()
                    os.replace(out / 'clock-alignment.json', out / 'clock-after.json')
            except Exception as error:
                outcome['cleanup_errors'].append('Windows clock: ' + repr(error))
            try:
                ctl.cleanup()
            except Exception as error:
                outcome['cleanup_errors'].append('owned Windows cleanup: ' + repr(error))
            if client_receipt:
                try:
                    if client and client.poll() is None:
                        outcome['client_stop'] = process_action(out / 'client-owned.json', owner, signal.SIGTERM)
                        try:
                            client.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            outcome['client_kill'] = process_action(out / 'client-owned.json', owner, signal.SIGKILL)
                            client.wait(timeout=5)
                except Exception as error:
                    outcome['cleanup_errors'].append('owned client cleanup: ' + repr(error))
            try:
                ctl.fetch()
            except Exception as error:
                outcome['cleanup_errors'].append('owned Windows fetch: ' + repr(error))
            try:
                network(out / 'network-final-zero.json', 'change', 0, initial['network']['state'])
            except Exception as error:
                outcome['network_ownership_or_cleanup_failed'] = True
                outcome['cleanup_errors'].append('owned network reset: ' + repr(error))
        for label, child in [('collector', collector), ('client Docker exec', client)]:
            try:
                if child:
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        outcome[label + '_host_stop'] = stop_host_process(child)
                    outcome[label + '_exit_code'] = child.returncode
            except Exception as error:
                outcome['cleanup_errors'].append(label + ': ' + repr(error))
        for stream in (collector_log, client_log):
            if stream:
                stream.close()
        outcome['finished_utc'] = utc()
        outcome['host_load_after_trial'] = list(os.getloadavg())
        if client:
            save(out / 'client-exit.json', dict(exit_code=client.returncode, ended_unix_s=time.time(),
                                               planned_stop_after_measurement=bool((outcome.get('client_stop') or outcome.get('client_kill')) and outcome.get('outcome') == 'COMPLETE'),
                                               cleanup_requested_stop=bool(outcome.get('client_stop') or outcome.get('client_kill')),
                                               scope='Docker exec final status; measured native survival is in ramp/campaign.json'))
        save(out / 'trial-result.json', outcome)
    return outcome


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings', type=Path, default=BASE / 'settings.json')
    parser.add_argument('--output', type=Path, default=BASE / 'campaign')
    parser.add_argument('--pcap', action='store_true', help='optional encrypted UDP header captures at256-byte snap length')
    parser.add_argument('--save-ppm', action='store_true', default=True, help='retain first/last central snapshots (enabled for this comparison)')
    args = parser.parse_args()
    settings = checked_settings(args.settings)
    args.output = args.output.resolve()
    args.output.relative_to(BASE)
    args.output.mkdir(exist_ok=False)
    campaign = dict(schema='freerdp.windows_pr_comparison', version=1, started_utc=utc(),
                    settings=settings, settings_sha256=digest(args.settings),
                    coordinator_sha256=digest(__file__), scope='fresh production RDP transport connections; existing Windows login may be reused; no native benchmark ABI',
                    warmup_seconds=45, measurement_seconds=120,
                    impairment='owned upstream all-IPv4 VM ingress; large>=512 bytes get1ms delay/70% reorder plus selected loss; small packets selected loss; no rate cap',
                    evidence='Windows10s continuous PDH and1Hz visual hashes, not verified application goodput or client FPS',
                    completion_semantics='ramp COMPLETE means process survived the measured interval; initial UDP readiness and sampled visual progress remain separate evidence',
                    trials=[dict(trial_id=f'{variant}-loss{loss}-{uuid.uuid4().hex[:8]}', variant=variant, engine=variant, loss_percent=loss, outcome='NOT_RUN')
                            for index, loss in enumerate([0, 25, 50, 70])
                            for variant in (['local', 'pr'] if index % 2 == 0 else ['pr', 'local'])])
    for trial in campaign['trials']:
        trial['path'] = trial['trial_id']
    save(args.output / 'trial-index.json', dict(schema='freerdp.windows_pr_comparison_index', version=1, trials=[{key:trial[key] for key in ('trial_id','engine','loss_percent','path')} for trial in campaign['trials']]))
    save(args.output / 'campaign.json', campaign)
    initial = preflight(settings, args.output / 'preflight.json')
    h = host_control()
    # Reject getpass's echoing stdin fallback. Root executes this in a PTY.
    with warnings.catch_warnings():
        warnings.simplefilter('error', getpass.GetPassWarning)
        password = getpass.getpass('Windows benchmark account password: ')
    if not password or '\n' in password or '\r' in password:
        raise ValueError('a nonempty single-line interactive credential is required')
    try:
        for index, planned in enumerate(campaign['trials']):
            campaign['trials'][index] = run_trial(settings, h, args.output, planned['trial_id'], planned['variant'],
                                                  planned['loss_percent'], password, initial,
                                                  pcap=args.pcap, save_ppm=args.save_ppm)
            save(args.output / 'campaign.json', campaign)
            result = campaign['trials'][index]
            if result.get('interrupted') or result.get('network_ownership_or_cleanup_failed'):
                campaign['stopped_early_reason'] = 'interrupted or owned network validation/reset failed'
                break
    finally:
        password = None
        campaign['finished_utc'] = utc()
        save(args.output / 'campaign.json', campaign)
    return 0 if all(t['outcome'] == 'COMPLETE' for t in campaign['trials']) else 1


if __name__ == '__main__':
    raise SystemExit(main())

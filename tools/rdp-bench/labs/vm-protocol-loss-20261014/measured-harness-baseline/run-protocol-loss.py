#!/usr/bin/env python3
"""Fresh phase-labelled TCP/UDP/all-IPv4 loss comparisons in this dedicated lab.

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
HOST_HELPER = BASE / 'host-control.py'
NET = '/suite/upstream-method/linux-ingress.py'
RAMP = '/suite/upstream-method/run-ramp.py'
STATE = '/bench/line-state.json'
CONTAINER = 'freerdp-protocol-loss-20261014'
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


def capture_footer(text):
    spec = importlib.util.spec_from_file_location('vm_comparison_process_guard', BASE / 'process-guard.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.capture_footer(text)


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
        if not re.fullmatch('[0-9a-f]{40}', entry.get('commit', '')):
            raise ValueError('explicit declared source commit required')
        for field in ('binary', 'source_root', 'build_root'):
            value = entry.get(field)
            if not isinstance(value, str) or not value.startswith('/') or '..' in Path(value).parts:
                raise ValueError('explicit absolute binary/source/build paths without parent traversal required')
        files = entry.get('source_files')
        if not isinstance(files, list) or not files or any(not isinstance(x, str) or x.startswith('/') or '..' in Path(x).parts for x in files):
            raise ValueError('explicit relative source_files required')
    for name in ('owned-exec.py', 'measurement-driver.py', 'process-guard.py'):
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
    # PIDFD binds the signal to the verified original process. tcpdump UID drop
    # may hide /proc/PID/exe; only its exact-argv/comm guard permits that case.
    command = ['python3', '/bench/process-guard.py', '--receipt', container_path(receipt_path),
               '--owner', owner, '--signal', str(int(sig) if sig else 0)]
    return json.loads(docker(command).stdout)


def network(output, operation, loss=None, expected=None):
    identity_keys = ('owner', 'netns', 'interface', 'ingress', 'receive', 'mac', 'loss_protocol')
    if expected is not None:
        current = json.loads(docker(['python3', '-c', 'import pathlib;print(pathlib.Path("/bench/line-state.json").read_text())']).stdout)
        if any(current.get(key) != expected.get(key) for key in identity_keys):
            raise ValueError('owned network identity differs from preflight')
    command = ['python3', NET, '--state', STATE, '--output', container_path(output), operation]
    if loss is not None:
        command += ['--loss', str(loss)]
    docker(command)
    receipt = json.loads(Path(output).read_text())
    if operation != 'off' and expected is not None and any(receipt['state'].get(key) != expected.get(key) for key in identity_keys):
        raise ValueError('owned network identity changed during controller action')
    return receipt


def select_protocol(settings, output, protocol):
    """Change selector only between closed trial groups through guarded off/setup."""
    output.mkdir(exist_ok=False)
    current = json.loads(docker(['python3', '-c', 'import pathlib; p=pathlib.Path("/bench/line-state.json"); print(p.read_text() if p.exists() else "null")']).stdout)
    if current is not None:
        before = network(output / 'before.json', 'snapshot', expected=current)
        if current.get('loss_protocol') == protocol:
            return network(output / 'reset-zero.json', 'change', 0, before['state'])
        network(output / 'off.json', 'off', expected=before['state'])
    command = ['python3', NET, '--state', STATE, '--output', container_path(output / 'setup.json'),
               'setup', '--vm-ip', settings['vm_ip'], '--interface', settings.get('network_interface', 'eth0'),
               '--loss-protocol', protocol]
    docker(command)
    receipt = json.loads((output / 'setup.json').read_text())
    if receipt['state'].get('loss_protocol') != protocol or receipt['state'].get('vm_ip') != settings['vm_ip']:
        raise ValueError('network setup selector/source differs from requested protocol')
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
    code = r'''import hashlib,json,os,pathlib,shutil,sys
cfg=json.loads(sys.argv[1]);result={'kernel':os.uname().release,'variants':{},'display_socket_present':pathlib.Path('/tmp/.X11-unix/X99').is_socket(),'image_script_present':pathlib.Path('/bench/image-progress.py').is_file(),'suite_helpers':{},'tcpdump':shutil.which('tcpdump')}
for n in ['linux-ingress.py','run-ramp.py']:
 p=pathlib.Path('/suite/upstream-method')/n;assert p.is_file(),str(p);result['suite_helpers'][n]=hashlib.sha256(p.read_bytes()).hexdigest()
for key,v in cfg['variants'].items():
 binary=pathlib.Path(v['binary']);assert binary.is_file() and os.access(binary,os.X_OK),str(binary)
 src=pathlib.Path(v['source_root']);names=v['source_files']
 entry={'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'source_sha256':{},'libraries_sha256':{}}
 for name in names:
  p=src/name
  if not p.is_file() and '/' not in name:p=src/'libfreerdp/core'/name
  assert p.is_file(),str(p);entry['source_sha256'][str(p.relative_to(src))]=hashlib.sha256(p.read_bytes()).hexdigest()
 build=pathlib.Path(v['build_root'])
 for pattern in ['libfreerdp3.so.*','libfreerdp-client3.so.*','libwinpr3.so.*']:
  paths=sorted(build.rglob(pattern));assert paths,pattern
  for p in paths:entry['libraries_sha256'][str(p.relative_to(build))]=hashlib.sha256(p.read_bytes()).hexdigest()
 result['variants'][key]=entry
assert result['display_socket_present'],'owned Xvfb socket missing'
assert result['image_script_present'],'image observer script missing'
print(json.dumps(result))'''
    payload = dict(variants=settings['variants'])
    receipt = json.loads(docker(['python3', '-c', code, json.dumps(payload)], timeout=90).stdout)
    receipt['host_helpers_sha256'] = {str(path.relative_to(ROOT)): digest(path)
                                       for path in (Path(__file__).resolve(), BASE / 'owned-exec.py',
                                                    BASE / 'measurement-driver.py', BASE / 'process-guard.py', HOST_HELPER)}
    receipt['settings_sha256'] = digest(output.parent / 'settings.json') if (output.parent / 'settings.json').is_file() else None
    receipt['utc'] = utc()
    receipt['network'] = network(output.with_name('network-preflight.json'), 'snapshot')
    save(output, receipt)
    return receipt


def run_trial(settings, h, output, trial_id, variant, loss, password, initial, *, phase, protocol, trace=False, pcap=False, capture_max_packets=1000000, save_ppm=True):
    out = output / trial_id
    out.mkdir(exist_ok=False)
    args = SimpleNamespace(ssh_target=settings['username']+'@'+settings['vm_ip'], ssh_socket=settings['ssh_socket'],
                           windows_root=str(PureWindowsPath(settings['windows_scripts_root']) / trial_id),
                           scripts_root=settings['windows_scripts_root'],
                           task_name='RdpBench-Pr13608-protocol-' + trial_id)
    cfg = h.checked_config(args)
    ctl = h.Controller(cfg, out)
    client = collector = None
    client_log = collector_log = None
    client_receipt = None
    captures = {}
    outcome = dict(trial_id=trial_id, variant=variant, engine=variant, path=trial_id, loss_percent=loss,
                   source_commit=settings['variants'][variant]['commit'], phase_id=phase, protocol=protocol,
                   performance_eligible=not trace and not pcap, diagnostic_trace=trace, started_utc=utc(),
                   requested_warmup_s=45, requested_measurement_s=120, outcome='FAILED_SETUP',
                   errors=[], cleanup_errors=[], host_load_before_trial=list(os.getloadavg()), scope='fresh live RDP transport connection; existing Windows login may be reused; server PDH and sampled visual progress')
    save(out / 'attempt.json', outcome)
    try:
        ctl.prepare()
        ctl.read_state()
        owner = ctl.state['owner']
        before = network(out / 'network-warmup-zero.json', 'change', 0, initial['network']['state'])
        if before['state'].get('loss_protocol') != protocol:
            raise ValueError('trial selector differs from declared protocol')
        binary = settings['variants'][variant]['binary']
        client_arguments = ['/v:' + settings['vm_ip'], '/u:' + settings['username'],
                            '/from-stdin:force', '/cert:fingerprint:sha256:' + settings['certificate_sha256'],
                            '/size:1280x800', '/bpp:32', '/gfx:progressive', '/network:auto', '/timeout:20000',
                            '/client-hostname:vmcmp-' + uuid.uuid4().hex[:8],
                            '/log-level:INFO', '/log-filters:' + LOG_FILTERS]
        if settings.get('domain'):
            client_arguments.insert(2, '/d:' + settings['domain'])
        trace_env = []
        if trace:
            trace_env = ['RDPEUDP_RECOVERY_TRACE='+container_path(out/'recovery-trace.jsonl'),
                         'RDPEUDP_RECOVERY_TRACE_ARM='+container_path(out/'trace.arm'),
                         'RDPEUDP_RECOVERY_TRACE_LIMIT=1048576']
        launch = ['docker', 'exec', '-i', CONTAINER, 'env', '-u', 'RDPEUDP_RECOVERY_TRACE',
                  '-u', 'RDPEUDP_RECOVERY_TRACE_ARM', '-u', 'RDPEUDP_RECOVERY_TRACE_LIMIT',
                  *trace_env, 'DISPLAY=:99', 'WLOG_LEVEL=INFO',
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
        if settings.get('graceful_collector_stop', False):
            collector_command.append('--graceful-stop')
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
            if trace and time.monotonic()-warmup_start >= 40 and not (out/'trace.arm').exists():
                (out/'trace.arm').touch(exist_ok=False)
                save(out/'trace-arm-request.json', dict(utc=utc(), requested_warmup_elapsed_s=time.monotonic()-warmup_start,
                                                      scope='arm five seconds before loss; native arm polling/dump coverage recorded separately'))
            time.sleep(max(0, min(1, 45 - (time.monotonic() - warmup_start))))
        provenance = dict(client_pid=client_receipt['pid'], client_start_ticks=client_receipt['start_ticks'],
                          owner=owner, variant=variant, source_commit=outcome['source_commit'],
                          phase_id=phase, protocol=protocol, performance_eligible=not trace and not pcap,
                          source_label=settings['variants'][variant].get('label'), trace_enabled=trace,
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
                          scope='selected '+protocol+' IPv4 VM ingress; other protocol bypass; no rate cap; not verified application goodput')
        save(out / 'provenance.json', provenance)
        if pcap:
            if not initial.get('tcpdump'):
                raise RuntimeError('optional packet capture requested but tcpdump unavailable')
            for label, interface in [('offered', before['state']['interface']), ('received', before['state']['receive'])]:
                log = (out / (label + '-capture.log')).open('xb')
                receipt_path = out / (label + '-capture-owned.json')
                command = ['docker', 'exec', CONTAINER, 'python3', '/bench/owned-exec.py', '--owner', owner,
                           '--receipt', container_path(receipt_path), '--executable', initial['tcpdump'], '--',
                           '-i', interface, '-U', '-nn', '-s', '256', '-c', str(capture_max_packets), '-w', container_path(out / (label + '.pcap')),
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
                    footer = capture_footer(text)
                    capture_health[label] = dict(exit_code=code, **footer,
                                                terminal_footer_complete=code == 0 and footer['kernel_drops'] is not None,
                                                complete=code == 0 and footer['kernel_drops'] is not None and footer['captured_packets'] != capture_max_packets,
                                                wire_evidence_valid=code == 0 and footer['kernel_drops'] == 0 and footer['captured_packets'] != capture_max_packets,
                                                capture_max_packets=capture_max_packets,
                                                max_file_bytes=24+272*capture_max_packets,
                                                capture_limit_reached=footer['captured_packets'] == capture_max_packets)
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


def planned_trials(phase, protocols, losses, variants):
    return [dict(trial_id=f'{phase}-{protocol}-{variant}-loss{loss}-{uuid.uuid4().hex[:8]}',
                 phase_id=phase, protocol=protocol, variant=variant, engine=variant,
                 loss_percent=loss, outcome='NOT_RUN')
            for protocol in protocols for number, loss in enumerate(losses)
            for variant in ([x for x in ('local','pr') if x in variants] if number % 2 == 0
                            else [x for x in ('pr','local') if x in variants])]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings', type=Path, default=BASE / 'settings.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', required=True)
    parser.add_argument('--protocols', choices=['tcp','udp','all'], nargs='+', default=['tcp','udp'])
    parser.add_argument('--losses', type=int, choices=[0,25,50,70], nargs='+', default=[0,25,50,70])
    parser.add_argument('--variants', choices=['local','pr'], nargs='+', default=['local','pr'])
    parser.add_argument('--trace', action='store_true')
    parser.add_argument('--pcap', action='store_true')
    parser.add_argument('--capture-max-packets', type=int, default=1000000)
    args = parser.parse_args()
    if not re.fullmatch('[A-Za-z0-9_-]{1,48}', args.phase):
        raise ValueError('short explicit phase identity required')
    if any(len(values) != len(set(values)) for values in (args.protocols,args.losses,args.variants)):
        raise ValueError('duplicate matrix conditions')
    if not 1 <= args.capture_max_packets <= 2000000:
        raise ValueError('capture count bound must be1..2000000 packets')
    diagnostic = args.trace or args.pcap
    if diagnostic and (len(args.protocols) != 1 or args.protocols[0] not in ('udp','all') or args.losses != [70] or args.variants != ['pr']):
        raise ValueError('extra observers only allowed in separately labelled PR70 UDP/all diagnostic')
    if not diagnostic and (args.losses != [0,25,50,70] or set(args.variants) != {'local','pr'}):
        raise ValueError('performance campaign must retain the full four-loss/two-client matrix')
    settings = checked_settings(args.settings)
    args.output = args.output.resolve(); args.output.relative_to(BASE); args.output.mkdir(exist_ok=False)
    trials = planned_trials(args.phase,args.protocols,args.losses,args.variants)
    for trial in trials:
        trial['path'] = trial['trial_id']
    campaign = dict(schema='freerdp.windows_protocol_loss',version=1,phase_id=args.phase,started_utc=utc(),
                    settings=settings,settings_sha256=digest(args.settings),coordinator_sha256=digest(__file__),
                    protocols=args.protocols,loss_percent=args.losses,variants=args.variants,
                    trial_kind='diagnostic' if diagnostic else 'performance',performance_eligible=not diagnostic,
                    warmup_seconds=45,measurement_seconds=120,packet_capture_enabled=args.pcap,trace_enabled=args.trace,
                    scope='fresh RDP connections; selected VM-ingress protocol; return direction clean; no rate cap',
                    impairment='selected protocol large packets >=512 bytes get1ms delay/70% reorder plus loss; selected small packets loss; nonselected bypass',
                    trials=trials)
    save(args.output/'trial-index.json',dict(schema='freerdp.windows_protocol_loss_index',version=1,phase_id=args.phase,
                                          protocols=args.protocols,loss_percent=args.losses,variants=args.variants,
                                          trial_kind=campaign['trial_kind'],trials=[{key:t[key] for key in ('trial_id','phase_id','engine','protocol','loss_percent','path')} for t in trials]))
    save(args.output/'run-configuration.json',dict(source_sha256=digest(__file__),phase_id=args.phase,
                                                  packet_capture_enabled=args.pcap,trace_enabled=args.trace,
                                                  performance_eligible=not diagnostic,display_image_sampling_hz=1,
                                                  server_counter_interval_s=10,classification=campaign['trial_kind']))
    save(args.output/'campaign.json',campaign)
    h=host_control()
    with warnings.catch_warnings():
        warnings.simplefilter('error',getpass.GetPassWarning)
        password=getpass.getpass('Windows benchmark account password: ')
    if not password or '\n' in password or '\r' in password:
        raise ValueError('nonempty single-line interactive credential required')
    initial=None; current_protocol=None
    try:
        for number,planned in enumerate(campaign['trials']):
            protocol=planned['protocol']
            if protocol != current_protocol:
                line=select_protocol(settings,args.output/('network-'+protocol),protocol)
                if initial is None:
                    initial=preflight(settings,args.output/'preflight.json')
                    save(args.output/'frozen-variants.json',dict(phase_id=args.phase,declared=settings['variants'],observed=initial['variants']))
                initial['network']=line;current_protocol=protocol
            result=run_trial(settings,h,args.output,planned['trial_id'],planned['variant'],planned['loss_percent'],
                             password,initial,phase=args.phase,protocol=protocol,trace=args.trace,pcap=args.pcap,
                             capture_max_packets=args.capture_max_packets)
            campaign['trials'][number]=result;save(args.output/'campaign.json',campaign)
            if result.get('interrupted') or result.get('network_ownership_or_cleanup_failed') or result.get('cleanup_errors'):
                campaign['stopped_early_reason']='interrupted or owned lifecycle cleanup/validation failed';break
    finally:
        password=None;campaign['finished_utc']=utc();save(args.output/'campaign.json',campaign)
    return 0 if all(t['outcome']=='COMPLETE' for t in campaign['trials']) else 1


if __name__ == '__main__':
    raise SystemExit(main())

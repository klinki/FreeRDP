#!/usr/bin/env python3
"""Release only this closed lab's receipt-identified runtime resources."""
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess

BASE = Path(__file__).resolve().parent

def run(argv, timeout=30):
    r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f'{argv[0]} failed ({r.returncode}): {r.stderr[-1000:]}')
    return r.stdout.strip()

def main():
    if (BASE / 'resource-cleanup.json').exists():
        raise RuntimeError('cleanup receipt exists; inspect rather than repeat blindly')
    phases = ['campaign-baseline', 'campaign-fixed', 'campaign-fixed-all',
              'diagnostic-pr70-udp', 'diagnostic-pr70-all', 'diagnostic-pr70-udp-buffered']
    closed = []
    controllers = []
    collector_wait_timeouts = []
    for name in phases:
        campaign = json.loads((BASE / name / 'campaign.json').read_text())
        assert campaign.get('finished_utc'), f'{name} is still open'
        for trial in campaign['trials']:
            assert not trial.get('errors') and not trial.get('cleanup_errors'), trial['trial_id']
            directory = BASE / name / trial['path']
            cleanup = json.loads((directory / 'cleanup.json').read_text())
            status=cleanup['collector_stop']['status']
            assert status in {'GRACEFUL_FOOTER_RECEIVED','GRACEFUL_STOP_TIMEOUT'}
            rows=[json.loads(line) for line in (directory/'counters.jsonl').read_text(encoding='utf-8-sig').splitlines() if line.strip()]
            footers=[row for row in rows if row.get('type')=='collector_end']
            assert len(footers)==1 and footers[0].get('requested_stop') and not footers[0].get('terminal_error')
            if status=='GRACEFUL_STOP_TIMEOUT':
                collector_wait_timeouts.append(dict(trial_id=trial['trial_id'],
                    wait_receipt=cleanup['collector_stop'],final_footer=footers[0],
                    qualification='Bounded footer observation timed out; fetched final footer and exact guest absence are verified separately.'))
            controllers.append(json.loads((directory / 'controller.json').read_text()))
        closed.append(dict(phase=name, trials=len(campaign['trials'])))
    spec = importlib.util.spec_from_file_location('owned_host_control', BASE / 'host-control.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    first = controllers[0]
    ctrl = module.Controller(first['config'], BASE / 'campaign-baseline' / campaign_path(first))
    names={c['config']['task_name'] for c in controllers}
    expression = ("$all=Get-ScheduledTask -ErrorAction Stop; "
                  "$tasks=@($all|Where-Object {$_.TaskName -like 'RdpBench-Pr13608-protocol-*'}|"
                  "Select-Object TaskName,State); $procs=@(Get-CimInstance Win32_Process|"
                  "Where-Object {$_.CommandLine -like '*C:\\RdpBenchProtocol20261014*'}|"
                  "Select-Object ProcessId,Name,CreationDate); "
                  "@{tasks=$tasks;processes=$procs;utc=[DateTime]::UtcNow.ToString('o')}|"
                  "ConvertTo-Json -Depth 5 -Compress")
    guest = json.loads(ctrl.ps(expression))
    owned=[task for task in guest['tasks'] if task['TaskName'] in names]
    guest['nonowned_prefix_tasks_ignored']=len(guest['tasks'])-len(owned)
    guest['tasks']=owned
    assert not guest['tasks'] and not guest['processes'], 'owned guest resources remain'
    runtime = json.loads((BASE / 'runtime-setup.json').read_text())
    inspection = json.loads(run(['docker','inspect',runtime['container_id']]))[0]
    assert inspection['Id'] == runtime['container_id']
    assert inspection['Name'] == '/' + runtime['container_name']
    assert inspection['Image'] == runtime['image_id']
    assert inspection['Config']['Labels']['org.freerdp.benchmark.owner'] == runtime['owner']
    power = json.loads((BASE / 'power-assertion-renewal.json').read_text())
    identity = run(['ps','-p',str(power['pid']),'-o','pid=,lstart=,args=']).strip()
    assert identity == power['ps_identity'].strip(), 'power assertion identity changed'
    run(['docker','exec',runtime['container_id'],'python3',
                             '/suite/upstream-method/linux-ingress.py','--state','/bench/line-state.json',
                             '--output','/bench/network-cleanup.json','off'])
    run(['docker','stop','--time','10',runtime['container_id']], timeout=25)
    run(['docker','rm',runtime['container_id']])
    ssh = first['config']
    run(['ssh','-S',ssh['ssh_socket'],'-O','exit',ssh['ssh_target']])
    os.kill(power['pid'],signal.SIGTERM)
    result = dict(schema='freerdp.protocol_lab_cleanup', version=1,
                  utc=datetime.now(timezone.utc).isoformat(), closed_phases=closed,
                  guest=guest, container_id=runtime['container_id'], container_removed=True,
                  collector_wait_timeouts=collector_wait_timeouts,
                  network_receipt='network-cleanup.json', ssh_control_socket_closed=ssh['ssh_socket'],
                  power_assertion_pid_stopped=power['pid'], persistent_power_settings_changed=False)
    (BASE / 'resource-cleanup.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

def campaign_path(controller):
    # Find the exact metadata receipt; do not infer a trial directory from a PID.
    for p in (BASE / 'campaign-baseline').glob('*/controller.json'):
        if json.loads(p.read_text()) == controller:
            return p.parent.name
    raise RuntimeError('matching controller receipt missing')

if __name__ == '__main__':
    main()

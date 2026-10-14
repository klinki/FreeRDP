#!/usr/bin/env python3
"""Per-trial Windows workload/counter control over an already authenticated SSH socket.

No passwords, client launch, Docker lifecycle or network shaping are handled.
Each trial has unique Windows/local outputs. The collector command stays in the
foreground; the calling agent owns that host process and its bounded lifetime.
"""
from __future__ import annotations
import argparse
import base64
from datetime import datetime
import hashlib
import json
from pathlib import Path, PureWindowsPath
import re
import stat
import subprocess
import time
import uuid


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def checked_config(args):
    for value in (args.windows_root, args.scripts_root):
        path = PureWindowsPath(value)
        if not path.is_absolute() or any(part in ('.', '..') for part in path.parts) or '"' in value:
            raise ValueError('absolute Windows paths without parent traversal required')
    if not re.fullmatch(r'RdpBench-Pr13608-[A-Za-z0-9_-]+', args.task_name):
        raise ValueError('unique owned task name required')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+@[A-Za-z0-9_.:-]+', args.ssh_target):
        raise ValueError('explicit user@host SSH target required')
    socket = Path(args.ssh_socket)
    if socket.is_symlink() or not stat.S_ISSOCK(socket.stat().st_mode):
        raise ValueError('an existing authenticated SSH control socket is required')
    return dict(ssh_target=args.ssh_target, ssh_socket=str(Path(args.ssh_socket).resolve()),
                windows_root=str(PureWindowsPath(args.windows_root)),
                scripts_root=str(PureWindowsPath(args.scripts_root)), task_name=args.task_name)


def select_session(payload):
    """Select the actual active RDP session, then corroborate its PDH instance."""
    user = payload['username'].casefold()
    pattern = re.compile(r'^\s*>?\s*(\S+)\s+(rdp-tcp#(\d+))\s+(\d+)\s+Active\b', re.I)
    sessions = []
    for line in payload.get('quser', []):
        match = pattern.match(line)
        if match and match[1].casefold() == user:
            sessions.append(dict(username=match[1], session_name=match[2], session_id=int(match[4]),
                                 expected_counter_instance='rdp-tcp ' + match[3]))
    if len(sessions) != 1:
        raise ValueError('require exactly one active RDP session for the authenticated Windows user')
    session = sessions[0]
    actual = {match[1].casefold(): match[1] for path in payload.get('counter_paths', [])
              if (match := re.search(r'\(([^()]*)\)', path))}
    expected = session['expected_counter_instance']
    session['counter_instance'] = actual.get(expected.casefold())
    session['counter_instance_evidence'] = 'CORROBORATED' if session['counter_instance'] else 'NO_MATCHING_PDH_INSTANCE'
    return dict(payload, **session)


class Controller:
    def __init__(self, config, output):
        self.config = config
        self.output = Path(output)
        self.metadata = self.output / 'controller.json'
        self.state = None

    def path(self, name):
        return str(PureWindowsPath(self.config['windows_root']) / name)

    def script(self, name):
        return str(PureWindowsPath(self.config['scripts_root']) / name)

    def ps(self, expression, timeout=30):
        encoded = base64.b64encode(("$ErrorActionPreference='Stop'; " + expression).encode('utf-16le')).decode()
        args = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'ControlMaster=no',
                '-o', 'ControlPath=' + self.config['ssh_socket'], '-o', 'ProxyCommand=false', self.config['ssh_target'],
                'powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', encoded]
        result = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
        if result.returncode:
            raise RuntimeError('Windows control failed: ' + (result.stderr + result.stdout)[-2000:])
        return result.stdout.strip().lstrip('\ufeff')

    def read_state(self):
        self.state = json.loads(self.metadata.read_text())
        if self.state['config'] != self.config:
            raise ValueError('controller configuration differs from the per-trial receipt')
        return self.state

    def guard(self):
        state = self.read_state()
        return ("$owned=Get-Content -LiteralPath " + quote(self.path('ownership.json')) + " -Raw|ConvertFrom-Json; "
                "if($owned.owner -ne " + quote(state['owner']) + " -or $owned.task_name -ne " + quote(self.config['task_name'])
                + ") {throw 'Windows trial ownership differs'}; ")

    def prepare(self):
        self.output.mkdir(parents=True, exist_ok=True)
        if self.metadata.exists():
            raise ValueError('local controller receipt exists; refusing to overwrite')
        owner = uuid.uuid4().hex
        state = dict(schema_version=1, owner=owner, config=self.config,
                     host_control_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        # Preserve the preparation attempt even if remote setup fails.
        save(self.metadata, state)
        expression = "if(Test-Path -LiteralPath " + quote(self.config['windows_root']) + ") {throw 'Windows trial directory exists'}; "
        for name in ('load.ps1', 'Invoke-Workload.ps1', 'Collect-Counters.ps1'):
            expression += "if(!(Test-Path -LiteralPath " + quote(self.script(name)) + ")) {throw 'Missing benchmark script'}; "
        expression += "New-Item -ItemType Directory -Path " + quote(self.config['windows_root']) + "|Out-Null; "
        expression += "@{owner=" + quote(owner) + ";task_name=" + quote(self.config['task_name']) + ";created_utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Set-Content -LiteralPath " + quote(self.path('ownership.json')) + " -Encoding UTF8; "
        expression += "@{scripts=@(" + ','.join(quote(self.script(name)) for name in ('load.ps1','Invoke-Workload.ps1','Collect-Counters.ps1')) + ")|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLower()}}}|ConvertTo-Json -Depth 4 -Compress"
        state['prepare_receipt'] = json.loads(self.ps(expression))
        save(self.metadata, state)
        return state

    def discover(self):
        expression = self.guard() + "$sets=@(Get-Counter -ListSet * -ErrorAction SilentlyContinue|Where-Object {$_.CounterSetName -match 'RemoteFX'}); "
        expression += "@{username=$env:USERNAME;quser=@(quser);counter_paths=@($sets|ForEach-Object {$_.PathsWithInstances})}|ConvertTo-Json -Depth 5 -Compress"
        receipt = select_session(json.loads(self.ps(expression)))
        save(self.output / 'session-discovery.json', receipt)
        return receipt

    def clock(self):
        guard = self.guard(); samples=[]
        for _ in range(3):
            start=time.time(); remote=self.ps(guard + "[DateTime]::UtcNow.ToString('o')"); end=time.time()
            epoch=datetime.fromisoformat(remote.replace('Z','+00:00')).timestamp()
            samples.append(dict(host_start_unix_s=start,host_end_unix_s=end,windows_utc=remote,
                                offset_windows_minus_host_s=epoch-(start+end)/2,uncertainty_s=(end-start)/2))
        selected=min(samples,key=lambda row:row['uncertainty_s'])
        receipt=dict(method='UTC bounds around authenticated SSH PowerShell; shortest round trip',
                     uncertainty_ms=selected['uncertainty_s']*1000,samples=samples,selected=selected)
        save(self.output/'clock-alignment.json',receipt)
        return receipt

    def load(self, seconds):
        discovery=json.loads((self.output/'session-discovery.json').read_text())
        receipt=self.ps(self.guard() + '& ' + quote(self.script('Invoke-Workload.ps1'))
                        + ' -ScriptPath ' + quote(self.script('load.ps1')) + ' -ReceiptFile ' + quote(self.path('load-pid.json'))
                        + ' -TaskName ' + quote(self.config['task_name']) + ' -Owner ' + quote(self.state['owner'])
                        + ' -Seconds ' + str(seconds))
        save(self.output/'load-task.json',json.loads(receipt))
        for _ in range(20):
            value=self.ps(self.guard() + "if(Test-Path -LiteralPath " + quote(self.path('load-pid.json')) + ") {Get-Content -LiteralPath " + quote(self.path('load-pid.json')) + " -Raw}")
            if value:
                value=json.loads(value)
                if value.get('owner') != self.state['owner'] or value.get('session_id') != discovery['session_id']:
                    raise ValueError('workload process receipt differs from owner/active RDP session')
                save(self.output/'load-pid.json',value)
                return value
            time.sleep(.5)
        raise RuntimeError('workload did not write its PID receipt within the bounded wait')

    def collector(self, seconds, interval, graceful_stop=False):
        discovery=json.loads((self.output/'session-discovery.json').read_text())
        expression=self.guard() + "if(Test-Path -LiteralPath " + quote(self.path('collector-pid.json')) + ") {throw 'Collector receipt exists'}; "
        stop_file = self.path('collector-stop.request.json') if graceful_stop else None
        expression += "$self=Get-CimInstance Win32_Process -Filter ('ProcessId='+$PID); @{owner=" + quote(self.state['owner']) + ";pid=$PID;session_id=$self.SessionId;creation_utc=$self.CreationDate.ToUniversalTime().ToString('o');script=" + quote(self.script('Collect-Counters.ps1')) + ";stop_file=" + (quote(stop_file) if stop_file else '$null') + ";started_utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Set-Content -LiteralPath " + quote(self.path('collector-pid.json')) + " -Encoding UTF8; "
        expression += '& ' + quote(self.script('Collect-Counters.ps1')) + ' -OutFile ' + quote(self.path('counters.jsonl')) + ' -Seconds ' + str(seconds) + ' -Interval ' + str(interval)
        if discovery.get('counter_instance'):
            expression += ' -Instance ' + quote(discovery['counter_instance'])
        if stop_file:
            expression += ' -StopFile ' + quote(stop_file)
        # The caller owns this foreground SSH invocation; no detached host worker.
        return dict(outcome='COLLECTOR_RETURNED',stdout=self.ps(expression,timeout=seconds+90))

    def stop_collector(self, wait_seconds=15):
        """Request a footer-producing stop, then retain any bounded fallback need."""
        expression = self.guard() + "$p=" + quote(self.path('collector-pid.json')) + "; "
        expression += "if(!(Test-Path -LiteralPath $p)) {@{status='NO_COLLECTOR_RECEIPT'}|ConvertTo-Json -Compress;exit}; $r=Get-Content -LiteralPath $p -Raw|ConvertFrom-Json; "
        expression += "if(!$r.stop_file) {@{status='LEGACY_NO_STOP_FILE'}|ConvertTo-Json -Compress;exit}; "
        expression += "if($r.owner -ne " + quote(self.state['owner']) + " -or $r.stop_file -ne " + quote(self.path('collector-stop.request.json')) + ") {throw 'Collector stop ownership differs'}; "
        expression += "$process=Get-CimInstance Win32_Process -Filter ('ProcessId='+$r.pid); if($process -and ($process.Name -ne 'powershell.exe' -or $process.SessionId -ne $r.session_id -or $process.CreationDate.ToUniversalTime().ToString('o') -ne $r.creation_utc)) {throw 'Collector process identity differs'}; "
        expression += "if(!(Test-Path -LiteralPath $r.stop_file)) {@{owner=" + quote(self.state['owner']) + ";requested_utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Set-Content -LiteralPath $r.stop_file -Encoding UTF8}; @{status='STOP_REQUESTED';process_present=[bool]$process}|ConvertTo-Json -Compress"
        receipt = json.loads(self.ps(expression))
        if not isinstance(receipt, dict) or receipt.get('status') not in ('NO_COLLECTOR_RECEIPT', 'LEGACY_NO_STOP_FILE', 'STOP_REQUESTED'):
            raise ValueError('malformed collector stop response')
        if receipt['status'] != 'STOP_REQUESTED':
            save(self.output / 'collector-stop.json', receipt)
            return receipt
        started = time.monotonic(); deadline = started + wait_seconds
        footer = None
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            probe = self.guard() + "$p=" + quote(self.path('counters.jsonl')) + ";if(Test-Path -LiteralPath $p) {$last=Get-Content -LiteralPath $p -Tail 1;if($last) {$row=$last|ConvertFrom-Json;if($row.type -eq 'collector_end') {$last}}}"
            try:
                value = self.ps(probe, timeout=min(3, max(.2, remaining)))
                if value:
                    footer = json.loads(value); break
            except subprocess.TimeoutExpired:
                pass
            time.sleep(min(.25, max(0, deadline - time.monotonic())))
        receipt.update(status='GRACEFUL_FOOTER_RECEIVED' if footer else 'GRACEFUL_STOP_TIMEOUT',
                       elapsed_s=time.monotonic()-started, footer=footer,
                       fallback_owned_cleanup_required=footer is None)
        save(self.output / 'collector-stop.json', receipt)
        return receipt

    def cleanup(self):
        graceful = self.stop_collector()
        expression=self.guard() + "$actions=@();$task=Get-ScheduledTask -TaskName " + quote(self.config['task_name']) + " -ErrorAction SilentlyContinue; "
        expression += "if($task) {if($task.Actions.Count -ne 1 -or !$task.Actions[0].Arguments.Contains(" + quote(self.path('load-pid.json')) + ") -or !$task.Actions[0].Arguments.Contains(" + quote(self.state['owner']) + ")) {throw 'Task action differs from owned workload'}; Stop-ScheduledTask -TaskName " + quote(self.config['task_name']) + ";Unregister-ScheduledTask -TaskName " + quote(self.config['task_name']) + " -Confirm:$false;$actions+='owned task removed'}; "
        for name in ('load-pid.json','collector-pid.json'):
            expression += "if(Test-Path -LiteralPath " + quote(self.path(name)) + ") {$r=Get-Content -LiteralPath " + quote(self.path(name)) + " -Raw|ConvertFrom-Json;if($r.owner -ne " + quote(self.state['owner']) + " -or $r.pid -le 0) {throw 'Process receipt ownership differs'}; $p=Get-CimInstance Win32_Process -Filter ('ProcessId='+$r.pid);if($p) {if($p.Name -ne 'powershell.exe' -or $p.SessionId -ne $r.session_id -or $p.CreationDate.ToUniversalTime().ToString('o') -ne $r.creation_utc) {throw 'Process identity differs; refusing to stop'};Stop-Process -Id $r.pid;$actions+='owned " + name + " process stopped'}}; "
        expression += "@{owner=" + quote(self.state['owner']) + ";actions=$actions;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress"
        receipt=json.loads(self.ps(expression));receipt['collector_stop']=graceful;save(self.output/'cleanup.json',receipt)
        return receipt

    def fetch(self):
        expression=self.guard() + '$files=@();'
        for name in ('ownership.json','load-pid.json','load-pid.exit.json','collector-pid.json','counters.jsonl'):
            expression += "$p=" + quote(self.path(name)) + ";if(Test-Path -LiteralPath $p) {$bytes=[IO.File]::ReadAllBytes($p);$files+=@{name=" + quote(name) + ";base64=[Convert]::ToBase64String($bytes)}}; "
        expression += '@{files=$files}|ConvertTo-Json -Depth 4 -Compress'
        payload=json.loads(self.ps(expression));receipts=[]
        for row in payload['files']:
            if row.get('name') not in ('ownership.json','load-pid.json','load-pid.exit.json','collector-pid.json','counters.jsonl'):
                raise ValueError('unexpected remote artifact name')
            content=base64.b64decode(row['base64'],validate=True)
            (self.output/row['name']).write_bytes(content)
            receipts.append(dict(name=row['name'],bytes=len(content),sha256=hashlib.sha256(content).hexdigest()))
        save(self.output/'fetch-receipt.json',receipts)
        return receipts


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssh-target',required=True)
    parser.add_argument('--ssh-socket',required=True)
    parser.add_argument('--windows-root',required=True,help='unique per-trial Windows output directory')
    parser.add_argument('--scripts-root',required=True,help='already deployed load/wrapper/collector directory')
    parser.add_argument('--task-name',required=True)
    parser.add_argument('--output',type=Path,required=True,help='unique local trial directory')
    commands=parser.add_subparsers(dest='command',required=True)
    for name in ('prepare','discover','clock','cleanup','fetch','stop_collector'):commands.add_parser(name)
    load=commands.add_parser('load');load.add_argument('--seconds',type=int,default=180)
    collector=commands.add_parser('collector');collector.add_argument('--seconds',type=int,default=180);collector.add_argument('--interval',type=int,default=10);collector.add_argument('--graceful-stop',action='store_true')
    args=parser.parse_args()
    if hasattr(args,'seconds') and not 1<=args.seconds<=86400:parser.error('seconds must be 1..86400')
    if hasattr(args,'interval') and not 1<=args.interval<=60:parser.error('interval must be 1..60')
    control=Controller(checked_config(args),args.output)
    if args.command in ('load','collector'):
        result=getattr(control,args.command)(args.seconds,*([args.interval,args.graceful_stop] if args.command=='collector' else []))
    else:result=getattr(control,args.command)()
    print(json.dumps(result,indent=2,allow_nan=False))


if __name__=='__main__':main()

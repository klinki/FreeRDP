#!/usr/bin/env python3
"""Live owned Windows collector lifecycle smoke; no graphics-rate claim."""
from pathlib import Path, PureWindowsPath
import concurrent.futures
import importlib.util
import json
import time
from types import SimpleNamespace
import uuid

BASE=Path(__file__).resolve().parent

def main():
    settings=json.loads((BASE/'settings.json').read_text())
    spec=importlib.util.spec_from_file_location('smoke_host_control',BASE/'host-control.py')
    h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
    name='collector-smoke-'+uuid.uuid4().hex[:8];out=BASE/name;out.mkdir()
    cfg=h.checked_config(SimpleNamespace(ssh_target=settings['username']+'@'+settings['vm_ip'],
        ssh_socket=settings['ssh_socket'],windows_root=str(PureWindowsPath(settings['windows_scripts_root'])/name),
        scripts_root=settings['windows_scripts_root'],task_name='RdpBench-Pr13608-'+name))
    ctl=h.Controller(cfg,out);ctl.prepare();ctl.read_state()
    h.save(out/'session-discovery.json',dict(counter_instance=None,scope='collector lifecycle smoke only; no phase FPS or session-binding claim'))
    result=dict(scope='graceful collector-stop smoke; no workload or network impairment')
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        future=pool.submit(ctl.collector,30,1,True)
        try:
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                expression=ctl.guard()+'if(Test-Path -LiteralPath '+h.quote(ctl.path('collector-pid.json'))+") {'ready'}"
                if ctl.ps(expression):break
                time.sleep(.1)
            else:raise RuntimeError('collector PID receipt did not arrive')
            time.sleep(2)
            result['cleanup']=ctl.cleanup()
            result['collector_result']=future.result(timeout=20)
            result['fetch']=ctl.fetch()
            rows=[json.loads(line) for line in (out/'counters.jsonl').read_text().splitlines() if line.strip()]
            footer=rows[-1]
            assert footer['type']=='collector_end' and footer['requested_stop'] is True
            assert footer['terminal_error'] is None
            assert result['cleanup']['collector_stop']['status']=='GRACEFUL_FOOTER_RECEIVED'
            result.update(pass_validation=True,footer=footer,raw_rows=len(rows))
        finally:
            if not future.done():result['fallback_cleanup']=ctl.cleanup()
            h.save(out/'smoke-result.json',result)
    h.save(BASE/'collector-stop-smoke-receipt.json',dict(directory=str(out),result=result))
    print(json.dumps(dict(directory=str(out),passed=result['pass_validation'],footer=footer),indent=2))

if __name__=='__main__':main()

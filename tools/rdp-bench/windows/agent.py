#!/usr/bin/env python3
"""Interactive Windows workload agent. Headed browser; no GUI work launched in SSH's desktop.
Provision Python + a tested Playwright version and Edge/Chromium once, before scored runs.
Only fixed operations on the local fixture are accepted (not arbitrary script execution).
"""
import argparse,ctypes,functools,hashlib,http.server,importlib.metadata,json,os,platform,shutil,subprocess,sys,tempfile,threading,time,uuid
from pathlib import Path

def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,x):
    p=Path(p);tmp=p.with_suffix('.tmp');tmp.write_text(json.dumps(x),encoding='utf-8');os.replace(tmp,p)
def source_hashes(root):
    return {str(p.relative_to(root)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((root/'windows').glob('*')) if p.is_file() and p.suffix in ('.py','.ps1')}
def main():
    a=argparse.ArgumentParser();a.add_argument('--root',required=True);args=a.parse_args()
    root=Path(args.root);sid=ctypes.c_uint32()
    if not ctypes.windll.kernel32.ProcessIdToSessionId(os.getpid(),ctypes.byref(sid)):raise RuntimeError('cannot resolve session')
    if sid.value==0:raise RuntimeError('refusing Session 0: use interactive scheduled task')
    identity={'agent_uuid':uuid.uuid4().hex,'pid':os.getpid(),'windows_session_id':sid.value,'started_utc':time.time(),'windows_version':platform.platform(),'hostname':platform.node(),'python_version':sys.version,'playwright_version':importlib.metadata.version('playwright')}
    from playwright.sync_api import sync_playwright
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self,*args):pass
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(root/'fixtures')))
    threading.Thread(target=server.serve_forever,daemon=True).start();port=server.server_address[1]
    profile=None;browser=None;counter=None;counter_errors=None;last=None;log=None;active_run=None;next_poll=0;sequence=0;exit_request=None
    def emit(kind,**data):
        nonlocal sequence
        if log:
            sequence+=1
            log.write(json.dumps({'type':kind,'sequence':sequence,'monotonic_ns':time.monotonic_ns(),'identity':identity,**data})+'\n');log.flush()
    def drain(page):
        try:
            d=page.evaluate('window.rdpBench ? window.rdpBench.drainEvents() : null')
            if d:emit('fixture_events',**d)
        except Exception as e:emit('fixture_read_error',error=str(e))
    with sync_playwright() as pw:
        try:
            deadline=time.monotonic()+12*3600
            while time.monotonic()<deadline:
                command_file=root/'command.json'
                c=read(command_file) if command_file.exists() else {}
                if c.get('request_id') and c['request_id']!=last:
                    last=c['request_id'];error=None;answer={}
                    try:
                        op=c['op']
                        if op=='hello':
                            if browser or active_run:raise ValueError('agent already owns a run; exit it before starting another')
                            active_run=c['run_id']
                            if not active_run or active_run in ('.','..') or any(x not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-' for x in active_run):raise ValueError('run_id')
                            dest=root/'runs'/active_run;dest.mkdir(parents=True,exist_ok=False)
                            log=open(dest/'agent.jsonl','a',encoding='utf-8',buffering=1)
                            fixture_hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((root/'fixtures').glob('*')) if p.is_file()}
                            expected_hashes=c.get('fixture_sha256',{})
                            if expected_hashes and any(fixture_hashes.get(name)!=value for name,value in expected_hashes.items()):raise ValueError('Windows fixture hashes differ from controller')
                            actual_sources=source_hashes(root)
                            expected_sources=c.get('source_sha256',{})
                            if expected_sources and actual_sources!=expected_sources:raise ValueError('Windows agent/counter/task sources differ from controller')
                            options=c.get('agent_config',{});expected=options.get('expected_session_id')
                            if expected is not None and expected!=sid.value:raise ValueError('wrong interactive Windows session')
                            profile=tempfile.mkdtemp(prefix='rdpbench-browser-')
                            kwargs={'headless':False,'no_viewport':True,'args':['--start-fullscreen','--no-first-run']}
                            if options.get('browser_channel'):kwargs['channel']=options['browser_channel']
                            browser=pw.chromium.launch_persistent_context(profile,**kwargs);page=browser.pages[0] if browser.pages else browser.new_page()
                            page.on('pageerror',lambda error:emit('browser_page_error',error=str(error)))
                            page.goto(f'http://127.0.0.1:{port}/index.html',wait_until='load')
                            answer=page.evaluate('window.rdpBench.ready()');answer['browser_version']=browser.browser.version if browser.browser else None
                            answer['user_agent']=page.evaluate('navigator.userAgent')
                            answer['fixture_sha256']=fixture_hashes
                            answer['playwright_version']=identity['playwright_version']
                            answer['source_sha256']=actual_sources
                            path_file=root/'counter-paths.json'
                            counter_errors=open(dest/'counter-errors.txt','w')
                            counter=subprocess.Popen(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(root/'windows/Collect-Counters.ps1'),'-SessionId',str(sid.value),'-OutFile',str(dest/'windows-counters.jsonl'),'-CounterPathsFile',str(path_file)],stdout=subprocess.DEVNULL,stderr=counter_errors)
                        elif op=='start':
                            if c['run_id']!=active_run or not browser:raise ValueError('wrong run or browser not ready')
                            # Local pinned video integrity is mandatory when the video case is used.
                            cfg=c['configuration']
                            if cfg.get('scene')=='video':
                                clip=(root/'fixtures'/cfg['videoUrl']).resolve()
                                if not clip.is_relative_to((root/'fixtures').resolve()):raise ValueError('video path escaped fixtures')
                                expected=c.get('video_sha256')
                                if not expected or hashlib.sha256(clip.read_bytes()).hexdigest()!=expected:raise ValueError('missing/mismatched pinned video SHA256')
                            answer=page.evaluate('(c)=>window.rdpBench.start(c)',cfg)
                            page.bring_to_front()
                        elif c['run_id']!=active_run:raise ValueError('wrong run')
                        elif op=='mark':answer=page.evaluate('(n)=>window.rdpBench.mark(n)',c['phase_id'])
                        elif op=='status':
                            answer=page.evaluate('window.rdpBench.status()')
                            answer['source_sha256']=source_hashes(root)
                            answer['fixture_sha256']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((root/'fixtures').glob('*')) if p.is_file() and p.suffix in ('.html','.js')}
                            drain(page)
                        elif op=='stop':
                            page.evaluate('window.rdpBench.stop()');drain(page);answer={'stopped':True}
                        elif op=='exit':
                            if browser:
                                page.evaluate('window.rdpBench.stop()');drain(page)
                            exit_request=last;break
                        else:raise ValueError('unknown fixed operation')
                        emit('command_completed',request_id=last,operation=op,answer=answer)
                    except Exception as e:error=repr(e);emit('command_failed',request_id=last,error=error)
                    write(root/'status.json',{'request_id':last,'run_id':active_run,'identity':identity,'answer':answer,'error':error,'monotonic_ns':time.monotonic_ns()})
                if browser and time.monotonic()>=next_poll:drain(page);next_poll=time.monotonic()+1
                # Playwright wait processes browser events; normal sleep before browser startup only.
                if browser:page.wait_for_timeout(50)
                else:time.sleep(.1)
        finally:
            if counter:
                counter.terminate()
                try:counter.wait(5)
                except subprocess.TimeoutExpired:counter.kill();counter.wait()
            if counter_errors:counter_errors.close()
            if browser:browser.close()
            if profile:shutil.rmtree(profile,ignore_errors=True)
            emit('agent_stopped')
            if log:log.close()
            server.shutdown()
            # Acknowledge only after all artifact producers have closed. The
            # controller can now collect complete files without racing cleanup.
            if exit_request:write(root/'status.json',{'request_id':exit_request,'identity':identity,'answer':{'exited':True},'run_id':active_run})
if __name__=='__main__':main()

from pathlib import Path
import shlex,subprocess,concurrent.futures
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',required=True,type=Path);p.add_argument('--build',required=True,type=Path);p.add_argument('--output',required=True,type=Path);args=p.parse_args()
root=args.source.resolve();tmp=args.output.resolve();tmp.mkdir(parents=True,exist_ok=True);cwd=args.build.resolve()/'client/SDL/SDL3'
names=['TestSDLInputMapping','TestSDLMonitorScale','TestSDLRenderGeometry','TestSDLRenderWindow','TestSDLRenderMetrics','TestSDLUpdateQueue']
def run(name):
    flagfile=cwd/'CMakeFiles'/f'{name}.dir/flags.make';flags={k:v for k,v in [line.split(' = ',1) for line in flagfile.read_text().splitlines() if ' = ' in line]}
    tokens=shlex.split((cwd/'CMakeFiles'/f'{name}.dir/link.txt').read_text());objects=[];log=''
    for token in tokens:
        if not token.endswith('.o'): continue
        src=token.split('.dir/',1)[1][:-2];out=tmp/(name+'-'+Path(src).name+'.o')
        cmd=['c++','-std=c++17','-O1']+shlex.split(flags.get('CXX_DEFINES',''))+shlex.split(flags.get('CXX_INCLUDES',''))+['-c',str(root/'client/SDL/SDL3'/src),'-o',str(out)]
        r=subprocess.run(cmd,capture_output=True,text=True,cwd=cwd);log+=r.stdout+r.stderr
        if r.returncode: (tmp/(name+'.log')).write_text(log);return name,'COMPILE FAIL',log[-1000:]
        objects.append((token,str(out)))
    for old,new in objects: tokens[tokens.index(old)]=new
    tokens[tokens.index('-o')+1]=str(tmp/name)
    r=subprocess.run(tokens,capture_output=True,text=True,cwd=cwd);log+=r.stdout+r.stderr
    if not r.returncode:
        r=subprocess.run([str(tmp/name)],capture_output=True,text=True,cwd=tmp,timeout=30);log+=r.stdout+r.stderr
    (tmp/(name+'.log')).write_text(log)
    return name,'PASS' if r.returncode==0 else 'FAIL',log[-1000:] if r.returncode else ''
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    results=list(pool.map(run,names))
    for result in results: print(*result,flush=True)
raise SystemExit(0 if all(result[1]=='PASS' for result in results) else 1)

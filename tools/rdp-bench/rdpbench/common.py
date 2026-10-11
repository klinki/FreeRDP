"""Strict artifact IO, monotonic timing, and reproducibility metadata."""
from __future__ import annotations
import hashlib,json,math,os,platform,subprocess,sys,time
from pathlib import Path

def read_json(path):
    with open(path,encoding="utf-8-sig") as f: return json.load(f)
def canonical(x): return json.dumps(x,sort_keys=True,separators=(",",":"),allow_nan=False).encode()
def digest(x): return hashlib.sha256(canonical(x)).hexdigest()
def sha256(path):
    h=hashlib.sha256()
    with open(path,"rb") as f:
        for b in iter(lambda:f.read(1024*1024),b""): h.update(b)
    return h.hexdigest()
def write_json(path,x):
    p=Path(path);p.parent.mkdir(parents=True,exist_ok=True)
    t=p.with_suffix(p.suffix+".tmp");t.write_text(json.dumps(x,indent=2,allow_nan=False)+"\n");os.replace(t,p)
def rows(path):
    with open(path,encoding="utf-8") as f:
        for n,line in enumerate(f,1):
            if line.strip():
                try: yield json.loads(line)
                except json.JSONDecodeError as e: raise ValueError(f"{path}:{n}: invalid JSON: {e}") from e
class Journal:
    def __init__(self,path):
        Path(path).parent.mkdir(parents=True,exist_ok=True)
        self.f=open(path,"a",encoding="utf-8",buffering=1); self.sequence=0
    def emit(self,kind,**data):
        self.sequence+=1
        self.f.write(json.dumps(dict(type=kind,seq=self.sequence,t_ns=time.monotonic_ns(),**data),allow_nan=False)+"\n")
    def close(self): self.f.close()
def quantile(xs,q):
    """Nearest-rank percentile; no interpolation across timeouts/missing values."""
    a=sorted(xs)
    return a[max(0,min(len(a)-1,math.ceil(q*len(a))-1))] if a else None

def timing_summary(xs):
    return {"count":len(xs),"p50":quantile(xs,.5),"p95":quantile(xs,.95),"p99":quantile(xs,.99),"max":max(xs) if xs else None}

def environment():
    def cmd(args):
        try: return subprocess.check_output(args,stderr=subprocess.DEVNULL,text=True,timeout=3).strip()
        except (OSError,subprocess.SubprocessError):return None
    return {"system":platform.platform(),"machine":platform.machine(),"python":sys.version,
            "cpu":platform.processor(),"logical_cpus":os.cpu_count(),"host":platform.node(),
            "monotonic":vars(time.get_clock_info("monotonic")),"kernel":platform.release(),
            "utc":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime()),
            "load":list(os.getloadavg()) if hasattr(os,"getloadavg") else None,
            "cpu_model":cmd(["sysctl","-n","machdep.cpu.brand_string"]) if sys.platform=="darwin" else cmd(["sh","-c","grep -m1 'model name' /proc/cpuinfo"])}

def seal(folder):
    """Integrity manifest, NOT an authenticity signature. Write after closing producers."""
    root=Path(folder)
    files={str(p.relative_to(root)):sha256(p) for p in sorted(root.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.json'}
    write_json(root/'SHA256SUMS.json',{"version":1,"files":files})
    return files

def audit(folder):
    root=Path(folder); saved=read_json(root/'SHA256SUMS.json')['files']; errors=[]
    for path,h in saved.items():
        p=(root/path).resolve()
        if not p.is_relative_to(root.resolve()):errors.append(f"unsafe path: {path}");continue
        if not p.is_file() or sha256(p)!=h: errors.append(f"missing/changed: {path}")
    actual={str(p.relative_to(root)) for p in root.rglob('*') if p.is_file() and p.name!='SHA256SUMS.json'}
    errors += [f"unsealed: {p}" for p in sorted(actual-set(saved))]
    return errors

def sleep_until(deadline_ns):
    while True:
        left=(deadline_ns-time.monotonic_ns())/1e9
        if left<=0:return
        time.sleep(min(left,.05))

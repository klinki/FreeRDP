"""Operator acceptance is explicit and evidence-backed. Never self-certify a benchmark."""
import datetime,math
from pathlib import Path
from .common import sha256

def engine_sources(root):
    root=Path(root).resolve()
    names=['rdpbench/link.py','rdpbench/feedback.py','rdpbench/endpoint.py','rdpbench/type1.py',
           'rdpbench/native.py','rdpbench/common.py','rdpbench/calibration.py','linux/ns_lab.py',
           '../../libfreerdp/core/rdp_bench_private.inc']
    return {name:(root/name).resolve() for name in names}

def accepted(receipt,host,engine,root,rate,observed_pps,peer_sha256=None):
    if not receipt:return False,['no calibration acceptance receipt']
    if not isinstance(receipt,dict):return False,['invalid calibration receipt']
    problems=[]
    if receipt.get('approved') is not True or not isinstance(receipt.get('approved_by'),str) or not receipt['approved_by'].strip():problems.append('no named approval')
    if receipt.get('host')!=host or receipt.get('engine')!=engine:problems.append('wrong host/engine')
    try:
        until=datetime.datetime.fromisoformat(receipt['valid_until_utc'].replace('Z','+00:00'))
        if until.tzinfo is None or datetime.datetime.now(datetime.timezone.utc)>until:problems.append('expired/naive expiry')
    except (ValueError,KeyError,TypeError):problems.append('missing expiry')
    def covers(key,required):
        value=receipt.get(key,0)
        return not isinstance(value,bool) and isinstance(value,(int,float)) and math.isfinite(value) and value>0 and value>=required
    if not covers('max_rate_mbps',rate):problems.append('outside calibrated rate')
    if not covers('max_packet_rate',2*observed_pps):problems.append('less than 2x observed packet-rate headroom')
    if not peer_sha256 or receipt.get('peer_sha256')!=peer_sha256:problems.append('fixed peer fingerprint missing/mismatched')
    sources=engine_sources(root)
    for file,h in receipt.get('engine_files',{}).items():
        p=sources.get(file)
        if p is None or not p.is_file() or sha256(p)!=h:problems.append('engine changed: '+file)
    required=set(sources)
    if not required.issubset(receipt.get('engine_files',{})):problems.append('engine fingerprints missing')
    evidence=receipt.get('evidence',[])
    if len(evidence)<2:problems.append('need calibration and observer/instrumentation-overhead evidence')
    for e in evidence:
        p=Path(e.get('path',''))
        if not p.is_file() or sha256(p)!=e.get('sha256'):problems.append('missing/changed evidence')
    return not problems,problems

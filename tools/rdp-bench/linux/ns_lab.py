#!/usr/bin/env python3
"""Owned three-namespace lab. Never changes an existing host NIC/qdisc.
Requires Linux root / CAP_NET_ADMIN. Run on a dedicated CI worker or Linux VM.
"""
import argparse,json,math,os,re,subprocess,uuid
from pathlib import Path

def run(*args):return subprocess.check_output(args,stderr=subprocess.STDOUT,text=True).strip()
def ns(n,*args):return run('ip','netns','exec',n,*args)
def load(p):return json.loads(Path(p).read_text())
def apply(state,profile,seed):
    p=load(profile[0])['profiles'][profile[1]]
    if any(p[d].get('blackouts_ms') or p[d].get('reorder_extra_ms') for d in ('c2s','s2c')):
        raise ValueError('Timed blackouts / exact extra-delay reordering require the portable relay profile; this kernel adapter refuses to silently approximate them.')
    for iface,direction,s in [('left','s2c',seed+1000003),('right','c2s',seed)]:
        d=p[direction];rate=d['rate_mbps'];delay=d['one_way_ms'];q=d['queue_ms']
        # netem limit counts delayed packets too. This is an approximate queue-time budget.
        # Save the actual packet limit; packet-size distribution changes its time equivalent.
        limit=max(8,math.ceil(rate*1e6/8*(delay+q)/1000/1200)+4) if rate else 4096
        args=['tc','qdisc','replace','dev',iface,'root','netem','limit',str(limit)]
        if delay or d.get('jitter_ms'):args+=['delay',f'{delay}ms',f"{d.get('jitter_ms',0)}ms"]
        if d.get('loss'):args+=['loss','random',f"{d['loss']*100}%"]
        if d.get('duplicate'):args+=['duplicate',f"{d['duplicate']*100}%"]
        if d.get('reorder'):args+=['reorder',f"{d['reorder']*100}%"]
        if rate:args+=['rate',f'{rate}mbit']
        args+=['seed',str(s)]
        ns(state['router_ns'],*args)
    return json.loads(ns(state['router_ns'],'tc','-j','-s','qdisc','show'))

def main():
    a=argparse.ArgumentParser();a.add_argument('op',choices=['create','profile','show','destroy']);a.add_argument('--state',required=True)
    a.add_argument('--profiles');a.add_argument('--profile');a.add_argument('--seed',type=int,default=11);args=a.parse_args()
    if os.geteuid()!=0:raise SystemExit('requires root / a dedicated namespaced runner')
    p=Path(args.state)
    if args.op=='create':
        if p.exists():raise SystemExit('state exists; do not overwrite')
        token='rb'+uuid.uuid4().hex[:8]
        s={'left_ns':token+'l','router_ns':token+'m','right_ns':token+'r','left_ip':'10.203.1.1','right_ip':'10.203.2.1','owner_token':token}
        created=[];host_links=[];s['offload_results']=[]
        try:
            for key in ['left_ns','router_ns','right_ns']:run('ip','netns','add',s[key]);created.append(s[key]);ns(s[key],'ip','link','set','lo','up')
            for side,num in [('left',1),('right',2)]:
                end=s[side+'_ns'];router=s['router_ns'];dev=token+side[0]
                run('ip','link','add',dev,'type','veth','peer','name',dev+'p');host_links += [dev,dev+'p']
                run('ip','link','set',dev,'netns',end);run('ip','link','set',dev+'p','netns',router)
                ns(end,'ip','link','set',dev,'name','eth0');ns(router,'ip','link','set',dev+'p','name',side)
                ns(end,'ip','addr','add',f'10.203.{num}.1/24','dev','eth0');ns(end,'ip','link','set','eth0','up')
                ns(router,'ip','addr','add',f'10.203.{num}.254/24','dev',side);ns(router,'ip','link','set',side,'up')
                ns(end,'ip','route','add','default','via',f'10.203.{num}.254')
                # Offloads can alter the unit seen by netem. Record unsupported toggles.
                for name,iface in [(end,'eth0'),(router,side)]:
                    for off in ['gro','gso','tso']:
                        try:
                            output=ns(name,'ethtool','-K',iface,off,'off')
                            s['offload_results'].append({'namespace':name,'interface':iface,'feature':off,'disabled':True,'output':output})
                        except (OSError,subprocess.CalledProcessError) as e:
                            s['offload_results'].append({'namespace':name,'interface':iface,'feature':off,'disabled':False,'error':repr(e)})
            ns(s['router_ns'],'sysctl','-qw','net.ipv4.ip_forward=1')
            p.write_text(json.dumps(s,indent=2));print(p.read_text())
        except BaseException:
            for dev in host_links:
                try:run('ip','link','del',dev)
                except (OSError,subprocess.SubprocessError):pass
            for n in reversed(created):
                try:run('ip','netns','del',n)
                except subprocess.SubprocessError:pass
            raise
    else:
        s=load(p)
        if not re.fullmatch(r'rb[0-9a-f]{8}',s['owner_token']) or any(s[k]!=s['owner_token']+suffix for k,suffix in [('left_ns','l'),('router_ns','m'),('right_ns','r')]):raise SystemExit('invalid ownership metadata')
        if args.op=='destroy':
            for key in ['left_ns','right_ns','router_ns']:
                # Refuse to kill arbitrary user processes. Stop endpoints first.
                if run('ip','netns','pids',s[key]):raise SystemExit('namespace has running processes; stop them before destroy')
            for key in ['left_ns','right_ns','router_ns']:run('ip','netns','del',s[key])
            p.unlink()
        elif args.op=='profile':
            if not args.profiles or not args.profile:raise SystemExit('profile and profiles required')
            print(json.dumps(apply(s,(args.profiles,args.profile),args.seed)))
        else:print(ns(s['router_ns'],'tc','-j','-s','qdisc','show'))
if __name__=='__main__':main()

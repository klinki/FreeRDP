#!/usr/bin/env python3
"""Bounded read-only TCP socket snapshots for a separate diagnostic trial."""
import argparse
import datetime
import ipaddress
import json
import os
from pathlib import Path
import signal
import subprocess
import time

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--owner',required=True)
    parser.add_argument('--host',default='192.168.64.2')
    parser.add_argument('--seconds',type=float,default=240)
    args=parser.parse_args()
    host=str(ipaddress.IPv4Address(args.host))
    if not 1<=args.seconds<=600:raise ValueError('bounded1..600s diagnostic required')
    stopped=False
    def stop(signum,frame):
        nonlocal stopped
        stopped=True
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    start=time.monotonic();errors=0;count=0
    with args.output.open('x') as stream:
        def write(record):
            record.update(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),monotonic_ns=time.monotonic_ns())
            stream.write(json.dumps(record,separators=(',',':'))+'\n');stream.flush()
        write(dict(type='metadata',owner=args.owner,pid=os.getpid(),host=host,
                   argv=['ss','-tinop','dst',host],interval_s=1,requested_s=args.seconds,
                   scope='isolated container TCP socket state; diagnostic only; not per-response latency'))
        while not stopped and time.monotonic()-start<args.seconds:
            try:
                result=subprocess.run(['ss','-tinop','dst',host],capture_output=True,text=True,timeout=3)
                errors+=result.returncode!=0
                udp_table=Path('/proc/net/udp').read_text()
                udp_snmp=[line for line in Path('/proc/net/snmp').read_text().splitlines() if line.startswith('Udp:')]
                write(dict(type='sample',sequence=count,exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr,
                           udp_socket_table=udp_table,udp_snmp=udp_snmp,
                           udp_scope='whole isolated network namespace; socket inode/port identifies client; counters are not inherently per-client'))
            except Exception as error:
                errors+=1;write(dict(type='error',sequence=count,error=repr(error)))
            count+=1
            due=start+count
            if due>time.monotonic():time.sleep(min(due-time.monotonic(),1))
        write(dict(type='end',samples=count,errors=errors,requested_stop=stopped,elapsed_s=time.monotonic()-start))

if __name__=='__main__':main()

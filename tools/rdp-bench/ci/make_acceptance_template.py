#!/usr/bin/env python3
"""Make an UNAPPROVED receipt; never infer scientific acceptance from successful execution."""
import argparse,datetime,json,platform,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root))
from rdpbench.common import sha256,write_json
from rdpbench.calibration import engine_sources
p=argparse.ArgumentParser();p.add_argument('--engine',choices=['python-relay','netns','direct'],required=True);p.add_argument('--output',required=True);a=p.parse_args()
r={'version':1,'approved':False,'approved_by':'','host':platform.node(),'engine':a.engine,
   'valid_until_utc':'','max_rate_mbps':0,'max_packet_rate':0,
   'peer_sha256':'','engine_files':{f:sha256(path) for f,path in engine_sources(root).items()},'evidence':[],
   'required_review':['effective rate/delay/loss and routing','2x packet/burst headroom','fixed peer and application driver headroom','instrumentation on/off overhead','known baseline noise envelope'],
   'notes':'Fill only from retained real measurements. Attach >=2 hashed evidence files. A template is not acceptance.'}
write_json(a.output,r);print(a.output)

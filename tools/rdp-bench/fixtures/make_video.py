#!/usr/bin/env python3
"""Generate a real H.264 fixture once, then copy that EXACT file to every trial.
This is synthetic video content, but it exercises an actual video decoder/player.
The hash, not regeneration on a different FFmpeg build, defines identity.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',default='clip.mp4');p.add_argument('--seconds',type=int,default=30);p.add_argument('--size',default='1920x1080');p.add_argument('--fps',type=int,default=30);a=p.parse_args()
if not 1<=a.seconds<=300 or not 1<=a.fps<=120:raise SystemExit('invalid duration or rate')
subprocess.run(['ffmpeg','-nostdin','-y','-f','lavfi','-i',f'testsrc2=size={a.size}:rate={a.fps}',
    '-t',str(a.seconds),'-an','-c:v','libx264','-preset','veryfast','-crf','20','-pix_fmt','yuv420p','-threads','1',
    '-map_metadata','-1','-movflags','+faststart',a.output],check=True)
f=Path(a.output);receipt={'file':f.name,'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'fps':a.fps,'size':a.size,'seconds':a.seconds,
 'ffmpeg':subprocess.check_output(['ffmpeg','-version'],text=True).splitlines()[0],'note':'Video-adjacent marker != proof of full-image fidelity; add aligned visual checkpoints.'}
f.with_suffix('.receipt.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt,indent=2))

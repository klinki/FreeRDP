from pathlib import Path
import subprocess,base64,json,hashlib,uuid,urllib.request
BASE=Path(__file__).resolve().parent;ROOT=Path('/Users/david/.codex/worktrees/pr13608-flight-bench/FreeRDP');SOCKET='/private/tmp/freerdp-protocol-loss-20261014.sock';REMOTE='C:\\RdpBenchProtocol20261014'
def ps(s):
 args=['ssh','-S',SOCKET,'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','david@192.168.64.2','powershell.exe','-NoProfile','-EncodedCommand',base64.b64encode(s.encode('utf-16le')).decode()]
 r=subprocess.run(args,capture_output=True,text=True,timeout=35)
 if r.returncode:raise RuntimeError((r.stderr+r.stdout)[-1800:])
 return r.stdout.strip().lstrip('\ufeff')
query=r"""$ErrorActionPreference='Stop'; $g=$null; $bindingError=$null; try {$g=Get-CimInstance -Namespace root/cimv2/TerminalServices -ClassName Win32_TSGeneralSetting -Filter "TerminalName='RDP-tcp'"} catch {$bindingError=$_.Exception.Message}; $cs=@(Get-ChildItem 'Cert:\LocalMachine\Remote Desktop'); $c=$null; if($g -and $g.SSLCertificateSHA1Hash){$c=$cs|Where-Object{$_.Thumbprint -eq $g.SSLCertificateSHA1Hash}|Select-Object -First 1}; if(-not $c -and $cs.Count -eq 1){$c=$cs[0]}; if(-not $c){throw 'RDP certificate selection ambiguous'}; $sha=[Security.Cryptography.SHA256]::Create(); $hash=[BitConverter]::ToString($sha.ComputeHash($c.RawData)).Replace('-','').ToLower(); $os=Get-CimInstance Win32_OperatingSystem; $sys=Get-CimInstance Win32_ComputerSystem; @{hostname=$env:COMPUTERNAME;os=$os.Caption;version=$os.Version;logical_cpus=$sys.NumberOfLogicalProcessors;memory_bytes=$sys.TotalPhysicalMemory;certificate_sha256=$hash;certificate_sha1=$c.Thumbprint;certificate_count=$cs.Count;rdp_certificate_binding=$g.SSLCertificateSHA1Hash;binding_query_error=$bindingError;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress"""
try:
 identity=json.loads(ps(query));assert identity['hostname']=='WIN-RL37D50MR0J';(BASE/'vm-preflight-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
 print({'hostname':identity['hostname'],'os':identity['os'],'logical_cpus':identity['logical_cpus'],'memory_bytes':identity['memory_bytes'],'certificate_matches_previous':identity['certificate_sha256']=='4f04fd907faedebb45444bfd9d118675eb24ec3fa2bacb24ac38e431d9185081','binding_query_error':identity['binding_query_error']},flush=True)
 source='142434adf715c3f3352c0e6dd7b7ece90244aeec';folder=BASE/'upstream';folder.mkdir(exist_ok=True);receipts=[]
 for name in ('README.md','session.sh','line.sh','ramp.sh','load.ps1','counters.ps1'):
  url=f'https://raw.githubusercontent.com/OpenThinRdp/openthinrdp/{source}/tests/udp-bench/{name}'
  with urllib.request.urlopen(url,timeout=30) as response:data=response.read()
  (folder/name).write_bytes(data);receipts.append({'name':name,'url':url,'sha256':hashlib.sha256(data).hexdigest()})
 (BASE/'upstream-source.json').write_text(json.dumps({'source_commit':source,'files':receipts},indent=2)+'\n')
 owner=json.loads((BASE/'settings.json').read_text())['owner'] if (BASE/'settings.json').exists() else uuid.uuid4().hex;settings={'vm_ip':'192.168.64.2','username':'david','domain':identity['hostname'],'certificate_sha256':identity['certificate_sha256'],'ssh_socket':SOCKET,'windows_scripts_root':REMOTE,'owner':owner,'variants':{'local':{'commit':'79ee244a4d5b2d5a280e165be4c643478484bf9e','binary':'/bench/linux-local/client/X11/xfreerdp'},'pr':{'commit':'a306a67c198f4493d8d2000bc10efd75b2432020','binary':'/bench/linux-pr/client/X11/xfreerdp'}},'loss_percent':[0,25,50,70],'warmup_seconds':45,'measurement_seconds':120}
 (BASE/'settings.json').write_text(json.dumps(settings,indent=2)+'\n')
 print(ps("$ErrorActionPreference='Stop'; $p='"+REMOTE+"'; if(Test-Path -LiteralPath $p){$o=Get-Content -LiteralPath ($p+'\\owner.json') -Raw|ConvertFrom-Json; if($o.owner -ne '"+owner+"'){throw 'Benchmark root ownership differs'}}else{New-Item -ItemType Directory -Path $p|Out-Null}; [IO.File]::WriteAllText(($p+'\\owner.json'),'{\"owner\":\""+owner+"\"}'); Write-Output 'Created owned benchmark script directory'"),flush=True)
 deployed=[]
 for p in (folder/'load.ps1',ROOT/'tools/rdp-bench/upstream-method/Collect-Counters.ps1',ROOT/'tools/rdp-bench/upstream-method/Invoke-Workload.ps1'):
  data=p.read_bytes();remote=REMOTE+'\\'+p.name
  transfer=subprocess.run(['scp','-o','ControlPath='+SOCKET,'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes',str(p),'david@192.168.64.2:'+remote.replace('\\','/')],capture_output=True,text=True,timeout=30)
  if transfer.returncode:raise RuntimeError('Script transfer failed: '+transfer.stderr[-1500:])
  h=ps("(Get-FileHash -LiteralPath '"+remote+"' -Algorithm SHA256).Hash").lower();assert h==hashlib.sha256(data).hexdigest();deployed.append({'local':str(p),'remote':remote,'sha256':h})
 (BASE/'windows-script-deployment.json').write_text(json.dumps({'owner':owner,'files':deployed},indent=2)+'\n');print('Three Windows scripts deployed and hash-verified',flush=True)
except Exception as error:
 (BASE/'windows-preparation-error.json').write_text(json.dumps({'error':str(error)},indent=2)+'\n');print('Preparation failed:',str(error),flush=True);raise SystemExit(1)

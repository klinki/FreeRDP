# Instrument the exact OpenThinRDP workload with per-instance machine-readable PDH evidence.
# Runs locally on the Windows host. It neither starts the load nor changes the network.
param([Parameter(Mandatory=$true)][string]$OutFile,
      [ValidateRange(1,86400)][int]$Seconds=600,
      [ValidateRange(1,60)][int]$Interval=10,
      [string]$Instance='')
$ErrorActionPreference='Stop'
$definitions=@(
 @{metric='server_udp_bandwidth';object='RemoteFX Network';counter='Current UDP Bandwidth';unit='upstream kbps label; raw cooked counter'},
 @{metric='server_udp_sent_rate';object='RemoteFX Network';counter='UDP Sent Rate';unit='raw cooked counter'},
 @{metric='server_udp_rtt_ms';object='RemoteFX Network';counter='Current UDP RTT';unit='ms'},
 @{metric='server_loss_percent';object='RemoteFX Network';counter='Loss Rate';unit='percent'},
 @{metric='server_retransmission_percent';object='RemoteFX Network';counter='Retransmission Rate';unit='percent'},
 @{metric='server_output_fps';object='RemoteFX Graphics';counter='Output Frames/Second';unit='server frames/s'},
 @{metric='server_skipped_network_fps';object='RemoteFX Graphics';counter='Frames Skipped/Second - Insufficient Network Resources';unit='server frames/s'}
)
$discoveryErrors=@();$english=@{};$localized=@{}
try {
 $perflib='HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib'
 $en=@((Get-ItemProperty "$perflib\009" -Name Counter).Counter)
 for($i=0;$i -lt $en.Count-1;$i+=2) { $english[$en[$i+1]]=$en[$i] }
 $local=@((Get-ItemProperty "$perflib\CurrentLanguage" -Name Counter).Counter)
 for($i=0;$i -lt $local.Count-1;$i+=2) { $localized[$local[$i]]=$local[$i+1] }
} catch { $discoveryErrors+= $_.Exception.Message }
foreach($d in $definitions) {
 $object=$d.object;$counter=$d.counter
 if($english.ContainsKey($object) -and $localized.ContainsKey($english[$object])) { $object=$localized[$english[$object]] }
 if($english.ContainsKey($counter) -and $localized.ContainsKey($english[$counter])) { $counter=$localized[$english[$counter]] }
 $selector='*';if($Instance) {$selector=$Instance}
 $d.path="\$object($selector)\$counter";$d.localizedObject=$object;$d.localizedCounter=$counter
}
$available=@()
try { $available=@(Get-Counter -ListSet * -ErrorAction SilentlyContinue | Where-Object { $_.CounterSetName -match 'RemoteFX' } | ForEach-Object { @{name=$_.CounterSetName;description=$_.Description;paths=@($_.PathsWithInstances)} }) }
catch { $discoveryErrors+=$_.Exception.Message }
$writer=[IO.StreamWriter]::new($OutFile,$false,[Text.UTF8Encoding]::new($false))
$watch=[Diagnostics.Stopwatch]::StartNew()
try {
 $metadata=@{type='metadata';schema_version=1;utc=[DateTime]::UtcNow.ToString('o');counter_instance=$Instance;
  interval_s=$Interval;definitions=$definitions;available_sets=$available;discovery_errors=$discoveryErrors;
  notes=@('Server output FPS does not establish physical client display rate.','All per-instance samples are retained; the report rejects ambiguous sessions.','Raw cooked values and nonzero PDH statuses are retained.')}
 $writer.WriteLine(($metadata|ConvertTo-Json -Depth 10 -Compress));$writer.Flush()
 while($watch.Elapsed.TotalSeconds -lt $Seconds) {
  $begin=[DateTime]::UtcNow.ToString('o');$beginTicks=[Diagnostics.Stopwatch]::GetTimestamp()
  $row=@{type='sample';schema_version=1;interval_start_utc=$begin;interval_end_utc=$null;utc=$null;
   monotonic_start_ticks=$beginTicks;monotonic_end_ticks=$null;frequency=[Diagnostics.Stopwatch]::Frequency;
   counters=@();error=$null;errors=@();missing_metrics=@()}
  $samples=@();$counterErrors=@();$batches=@()
  try {
   $batches=@(Get-Counter -Counter @($definitions|ForEach-Object {$_.path}) -SampleInterval $Interval -MaxSamples 2 -ErrorAction SilentlyContinue -ErrorVariable counterErrors)
   if($batches.Count) { $samples=@($batches[-1].CounterSamples) }
   if($batches.Count -gt 1) { $row.interval_start_utc=$batches[0].Timestamp.ToUniversalTime().ToString('o') }
  } catch { $row.error=$_.Exception.Message }
  $row.errors=@($counterErrors|ForEach-Object {$_.Exception.Message})
  foreach($d in $definitions) {
   $matches=@($samples|Where-Object { ($_.Path -split '\\')[-1] -ieq $d.localizedCounter -and $_.InstanceName -ine '_total' })
   if(!$matches.Count) {$row.missing_metrics+=$d.metric}
   foreach($sample in $matches) {
    $value=$sample.CookedValue;if([double]::IsNaN($value) -or [double]::IsInfinity($value)) {$value=$null}
    $row.counters+=@{metric=$d.metric;path=$sample.Path;instance=$sample.InstanceName;value=$value;status=$sample.Status;unit=$d.unit}
   }
  }
  $row.interval_end_utc=[DateTime]::UtcNow.ToString('o')
  if($batches.Count -gt 1) { $row.interval_end_utc=$batches[-1].Timestamp.ToUniversalTime().ToString('o') }
  $row.utc=$row.interval_end_utc
  $row.monotonic_end_ticks=[Diagnostics.Stopwatch]::GetTimestamp()
  $writer.WriteLine(($row|ConvertTo-Json -Depth 8 -Compress));$writer.Flush()
  # Missing providers can make Get-Counter return immediately. Bound sampling even then.
  $elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-$beginTicks)/[Diagnostics.Stopwatch]::Frequency
  if($elapsed -lt $Interval) {Start-Sleep -Milliseconds ([int](1000*($Interval-$elapsed)))}
 }
} finally { $writer.Dispose() }

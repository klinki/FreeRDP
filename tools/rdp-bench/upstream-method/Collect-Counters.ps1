# Instrument the exact OpenThinRDP workload with per-instance machine-readable PDH evidence.
# Runs locally on the Windows host. It neither starts the load nor changes the network.
param([Parameter(Mandatory=$true)][string]$OutFile,
      [ValidateRange(1,86400)][int]$Seconds=600,
      [ValidateRange(1,60)][int]$Interval=10,
      [string]$Instance='',
      [string]$StopFile='')
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
# One PDH query remains open for the whole run. The extra set establishes a
# baseline; every subsequent set covers the interval since the previous one.
$maxSets=[int][Math]::Ceiling($Seconds/[double]$Interval)+1
$previousTimestamp=$null;$previousTicks=$null;$setCount=0;$errorCount=0
$pendingErrors=[Collections.Generic.List[string]]::new()
$cadences=[Collections.Generic.List[double]]::new()
$terminalError=$null
$script:requestedStop=$false
try {
 $metadata=@{type='metadata';schema_version=2;utc=[DateTime]::UtcNow.ToString('o');counter_instance=$Instance;
  interval_s=$Interval;requested_counter_span_s=$Seconds;max_counter_sets=$maxSets;
  sampling_mode='continuous Get-Counter stream; first set is baseline';
  optional_stop_file=$StopFile;
  collector_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLower();
  definitions=$definitions;available_sets=$available;discovery_errors=$discoveryErrors;
  notes=@('Server output FPS does not establish physical client display rate.','All per-instance samples are retained; the report rejects ambiguous sessions.','The first set has no rate interval and never contributes cooked values to measured rates.','Raw cooked values, actual sample intervals and nonzero PDH statuses are retained.')}
 $writer.WriteLine(($metadata|ConvertTo-Json -Depth 10 -Compress));$writer.Flush()
 Get-Counter -Counter @($definitions|ForEach-Object {$_.path}) -SampleInterval $Interval -MaxSamples $maxSets -ErrorAction Continue 2>&1 | ForEach-Object {
  $item=$_
  if($item -is [Management.Automation.ErrorRecord]) {
   $errorCount++;$message=$item.Exception.Message;$pendingErrors.Add($message)
   $errorRow=@{type='collector_error';schema_version=2;utc=[DateTime]::UtcNow.ToString('o');
    monotonic_ticks=[Diagnostics.Stopwatch]::GetTimestamp();frequency=[Diagnostics.Stopwatch]::Frequency;
    error=$message;error_id=$item.FullyQualifiedErrorId;category=[string]$item.CategoryInfo;
    last_sample_utc=if($null -ne $previousTimestamp) {$previousTimestamp.ToUniversalTime().ToString('o')} else {$null}}
   $writer.WriteLine(($errorRow|ConvertTo-Json -Depth 6 -Compress));$writer.Flush()
  } else {
   $setCount++;$end=$item.Timestamp;$endTicks=[Diagnostics.Stopwatch]::GetTimestamp()
   $baseline=$null -eq $previousTimestamp
   $span=if(!$baseline) {($end-$previousTimestamp).TotalSeconds} else {$null}
   if($null -ne $span) {$cadences.Add($span)}
   $row=@{type=if($baseline) {'baseline'} else {'sample'};schema_version=2;sequence=$setCount;
    interval_start_utc=if(!$baseline) {$previousTimestamp.ToUniversalTime().ToString('o')} else {$null};
    interval_end_utc=$end.ToUniversalTime().ToString('o');utc=$end.ToUniversalTime().ToString('o');
    observed_interval_s=$span;baseline=$baseline;
    monotonic_start_ticks=$previousTicks;monotonic_end_ticks=$endTicks;frequency=[Diagnostics.Stopwatch]::Frequency;
    counters=@();error=$null;errors=@($pendingErrors.ToArray());missing_metrics=@()}
   $pendingErrors.Clear()
   foreach($d in $definitions) {
    $matches=@($item.CounterSamples|Where-Object {($_.Path -split '\\')[-1] -ieq $d.localizedCounter -and $_.InstanceName -ine '_total'})
    if(!$matches.Count) {$row.missing_metrics+=$d.metric}
    foreach($sample in $matches) {
     $raw=$sample.CookedValue
     $finite=$null -ne $raw -and ![double]::IsNaN($raw) -and ![double]::IsInfinity($raw)
     if(!$finite) {$raw=$null}
     $valid=!$baseline -and $null -ne $sample.Status -and $sample.Status -eq 0 -and $finite
     $row.counters+=@{metric=$d.metric;path=$sample.Path;instance=$sample.InstanceName;
      value=if($valid) {$raw} else {$null};raw_cooked_value=$raw;valid_for_rate=$valid;
      status=$sample.Status;unit=$d.unit}
    }
   }
   $writer.WriteLine(($row|ConvertTo-Json -Depth 8 -Compress));$writer.Flush()
   $previousTimestamp=$end;$previousTicks=$endTicks
  }
  # Query disposal must pass through finally so the caller receives a flushed
  # collector_end receipt. Check after retaining the set/error that arrived.
  if($StopFile -and (Test-Path -LiteralPath $StopFile)) {
   $script:requestedStop=$true
   throw [OperationCanceledException]::new('RdpBench requested collector stop')
  }
 }
} catch {
 if(!$script:requestedStop) {
  $terminalError=$_.Exception.Message
  $writer.WriteLine((@{type='collector_error';schema_version=2;utc=[DateTime]::UtcNow.ToString('o');error=$terminalError;terminal=$true}|ConvertTo-Json -Compress));$writer.Flush()
  throw
 }
} finally {
 $cadenceSummary=if($cadences.Count) {$cadences|Measure-Object -Minimum -Maximum -Average} else {$null}
 $final=@{type='collector_end';schema_version=2;utc=[DateTime]::UtcNow.ToString('o');
  observed_counter_sets=$setCount;expected_counter_sets=$maxSets;nonterminal_errors=$errorCount;terminal_error=$terminalError;
  requested_stop=$script:requestedStop;
  stop_reason=if($script:requestedStop) {'REQUESTED_STOP'} elseif($terminalError) {'TERMINAL_ERROR'} else {'MAX_SAMPLES'};
  pending_errors=@($pendingErrors.ToArray());elapsed_s=$watch.Elapsed.TotalSeconds;
  cadence=@{n=$cadences.Count;min_s=if($cadenceSummary) {$cadenceSummary.Minimum} else {$null};
   max_s=if($cadenceSummary) {$cadenceSummary.Maximum} else {$null};mean_s=if($cadenceSummary) {$cadenceSummary.Average} else {$null}}}
 try {$writer.WriteLine(($final|ConvertTo-Json -Depth 6 -Compress));$writer.Flush()} finally {$writer.Dispose()}
}

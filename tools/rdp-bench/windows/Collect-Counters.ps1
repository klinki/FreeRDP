param([int]$SessionId,[string]$OutFile,[string]$CounterPathsFile='', [int]$Seconds=7200)
$ErrorActionPreference='Stop'
$paths=@()
if ($CounterPathsFile -and (Test-Path $CounterPathsFile)) { $paths=@(Get-Content $CounterPathsFile -Raw|ConvertFrom-Json) }
$writer=[IO.StreamWriter]::new($OutFile,$true,[Text.UTF8Encoding]::new($false))
$watch=[Diagnostics.Stopwatch]::StartNew()
try {
 while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
  $row=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');monotonicTicks=[Diagnostics.Stopwatch]::GetTimestamp();frequency=[Diagnostics.Stopwatch]::Frequency;sessionId=$SessionId;processes=@();counters=@();counterError=$null}
  $row.processes=@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.SessionId -eq $SessionId -and $_.Name -in @('msedge','chrome','dwm','python')} | ForEach-Object { @{pid=$_.Id;name=$_.Name;cpuSeconds=$_.CPU;workingSetBytes=$_.WorkingSet64;privateBytes=$_.PrivateMemorySize64} })
  if ($paths.Count) {
   try { $row.counters=@((Get-Counter -Counter $paths -MaxSamples 1 -ErrorAction Stop).CounterSamples | ForEach-Object { @{path=$_.Path;instance=$_.InstanceName;value=$_.CookedValue;status=$_.Status} }) }
   catch {$row.counterError=$_.Exception.Message}
  }
  $writer.WriteLine(($row|ConvertTo-Json -Depth 6 -Compress));$writer.Flush();Start-Sleep -Seconds 1
 }
} finally { $writer.Dispose() }

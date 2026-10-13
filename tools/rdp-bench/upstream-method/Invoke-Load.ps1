# Schedule only the supplied workload in the current user's interactive RDP session.
# Call after that user has logged in. Stop/remove the exact task name during cleanup.
param([Parameter(Mandatory=$true)][string]$ScriptPath,
      [Parameter(Mandatory=$true)][string]$TaskName,
      [ValidateRange(1,86400)][int]$Seconds=600)
$ErrorActionPreference='Stop'
$script=(Resolve-Path -LiteralPath $ScriptPath).Path
if($TaskName -notmatch '^RdpBench-Pr13608-[A-Za-z0-9_-]+$') { throw 'Use a unique owned RdpBench-Pr13608-* task name' }
if($script.Contains('"')) { throw 'Invalid script path' }
if(Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) { throw 'Task already exists; refusing to replace it' }
$user=[Security.Principal.WindowsIdentity]::GetCurrent().Name
$action=New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$script`" -Seconds $Seconds"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::FromSeconds($Seconds+60)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings | Out-Null
try { Start-ScheduledTask -TaskName $TaskName }
catch { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false; throw }
@{task_name=$TaskName;user=$user;script=$script;script_sha256=(Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash.ToLower();seconds=$Seconds;started_utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Compress

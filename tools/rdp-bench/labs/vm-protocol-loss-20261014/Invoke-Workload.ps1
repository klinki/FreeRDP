# Run the original upstream load unchanged, with an owned process receipt.
param([Parameter(Mandatory=$true)][string]$ScriptPath,
      [Parameter(Mandatory=$true)][string]$ReceiptFile,
      [Parameter(Mandatory=$true)][string]$TaskName,
      [Parameter(Mandatory=$true)][string]$Owner,
      [ValidateRange(1,86400)][int]$Seconds=180,
      [ValidateSet('Launch','Run')][string]$Mode='Launch')
$ErrorActionPreference='Stop'
if($TaskName -notmatch '^RdpBench-Pr13608-[A-Za-z0-9_-]+$') {throw 'Invalid owned task name'}
if($Owner -notmatch '^[a-f0-9]{32}$') {throw 'Invalid owner token'}
$script=(Resolve-Path -LiteralPath $ScriptPath).Path
foreach($path in @($script,$ReceiptFile,$PSCommandPath)) {if($path.Contains('"')) {throw 'Invalid script/receipt path'}}
if($Mode -eq 'Run') {
 if(Test-Path -LiteralPath $ReceiptFile) {throw 'Process receipt already exists'}
 $self=Get-CimInstance Win32_Process -Filter ('ProcessId='+$PID)
 $receipt=@{owner=$Owner;task_name=$TaskName;pid=$PID;creation_utc=$self.CreationDate.ToUniversalTime().ToString('o');
  session_id=$self.SessionId;started_utc=[DateTime]::UtcNow.ToString('o');script=$script;
  script_sha256=(Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash.ToLower();
  wrapper=$PSCommandPath;wrapper_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLower()}
 $receipt|ConvertTo-Json -Compress|Set-Content -LiteralPath $ReceiptFile -Encoding UTF8
 $completed=$false;$errorText=$null
 try { & $script -Seconds $Seconds; $completed=$true }
 catch {$errorText=$_.Exception.Message;throw}
 finally {
  @{owner=$Owner;pid=$PID;creation_utc=$receipt.creation_utc;ended_utc=[DateTime]::UtcNow.ToString('o');completed=$completed;error=$errorText}|
   ConvertTo-Json -Compress|Set-Content -LiteralPath ([IO.Path]::ChangeExtension($ReceiptFile,'.exit.json')) -Encoding UTF8
 }
 exit
}
if(Test-Path -LiteralPath $ReceiptFile) {throw 'Process receipt already exists'}
if(Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {throw 'Task exists; refusing to overwrite'}
$user=[Security.Principal.WindowsIdentity]::GetCurrent().Name
$arguments="-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Mode Run -ScriptPath `"$script`" -ReceiptFile `"$ReceiptFile`" -TaskName $TaskName -Owner $Owner -Seconds $Seconds"
$action=New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument $arguments
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::FromSeconds($Seconds+60)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings|Out-Null
try {Start-ScheduledTask -TaskName $TaskName}
catch {Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false;throw}
@{owner=$Owner;task_name=$TaskName;user=$user;script=$script;seconds=$Seconds;receipt_file=$ReceiptFile;started_utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress

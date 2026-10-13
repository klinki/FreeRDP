param(
 [Parameter(Mandatory=$true)][string]$User,
 [Parameter(Mandatory=$true)][string]$Python,
 [string]$Root='C:\RdpBench',
 [string]$TaskName='RdpBenchAgent'
)
$ErrorActionPreference='Stop'
if (!(Test-Path -LiteralPath $Python)) { throw "Python executable missing" }
$script=Join-Path $Root 'windows\agent.py'
if (!(Test-Path -LiteralPath $script)) { throw "Deploy the suite into $Root first" }
# Interactive token: NO stored Windows password, NO SYSTEM GUI in session 0.
$action=New-ScheduledTaskAction -Execute $Python -Argument ('"{0}" --root "{1}"' -f $script,$Root) -WorkingDirectory $Root
$principal=New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 12) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Write-Host "Task registered. It only runs when $User has an interactive session. Use one dedicated test session."

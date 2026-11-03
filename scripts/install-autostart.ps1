<#
.SYNOPSIS
    Registers win-ldac-a2dp to start at logon (and optionally removes it).

.DESCRIPTION
    Creates a per-user scheduled task that launches a2dp_source_demo.exe with your
    chosen address, quality and capture source.

    The link key is stored beside the exe, so -WorkDir must stay the same between
    runs or the headphones will need re-pairing.

.EXAMPLE
    .\install-autostart.ps1 -Addr AC:80:0A:F2:C5:A2 -Mode HQ
    .\install-autostart.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$Addr = 'AC:80:0A:F2:C5:A2',
    [ValidateSet('HQ', 'SQ', 'MQ')]
    [string]$Mode = 'HQ',
    [ValidateSet('system', 'tone')]
    [string]$Source = 'system',
    [string]$WorkDir = "$env:USERPROFILE\win-ldac-a2dp",
    [string]$Exe,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$TaskName = 'win-ldac-a2dp'

if ($Remove) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "Removed scheduled task '$TaskName'." -ForegroundColor Green
    } else {
        Write-Host "No task named '$TaskName'." -ForegroundColor Yellow
    }
    return
}

if (-not $Exe) { $Exe = Join-Path $WorkDir 'a2dp_source_demo.exe' }
if (-not (Test-Path $Exe)) { throw "Executable not found: $Exe (pass -Exe or -WorkDir)" }

$arguments = "--addr $Addr --mode $Mode --source $Source"
Write-Host "Task   : $TaskName"
Write-Host "Command: $Exe $arguments"
Write-Host "WorkDir: $WorkDir"

$action  = New-ScheduledTaskAction -Execute $Exe -Argument $arguments -WorkingDirectory $WorkDir
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                                         -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 `
                                         -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -Description 'Streams Windows audio to LDAC headphones over a WinUSB-claimed radio.' `
    -Force | Out-Null

Write-Host "Registered. It starts at next logon; run it now with:" -ForegroundColor Green
Write-Host "  Start-ScheduledTask -TaskName $TaskName"
Write-Host "Remove with: .\install-autostart.ps1 -Remove" -ForegroundColor Gray

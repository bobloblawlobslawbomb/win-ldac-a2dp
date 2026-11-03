# Starts the LDAC A2DP source.
#   .\run.ps1 -Addr AC:80:0A:F2:C5:A2 -Mode HQ
# Run it from the folder where the exe lives: the link key (btstack_<MAC>.tlv) is
# written to the current directory, so a different cwd means re-pairing.
[CmdletBinding()]
param(
    [string]$Addr = 'AC:80:0A:F2:C5:A2',
    [ValidateSet('HQ','SQ','MQ')]
    [string]$Mode = 'HQ',
    [string]$Exe  = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Exe) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $candidates = @(
        (Join-Path (Split-Path -Parent $here) 'dist\a2dp_source_demo.exe'),
        (Join-Path (Split-Path -Parent $here) 'a2dp_source_demo.exe'),
        (Join-Path $PWD 'a2dp_source_demo.exe')
    )
    $Exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Exe -or -not (Test-Path $Exe)) { throw "a2dp_source_demo.exe not found. Pass -Exe <path>." }

$claimed = @()
foreach ($d in (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue)) {
    $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    if ($svc -match '^winusb$' -and $d.Class -in @('Bluetooth','USB','USBDevice')) { $claimed += $d.FriendlyName }
}
if (-not $claimed) {
    Write-Host "WARNING: no WinUSB-claimed Bluetooth radio detected." -ForegroundColor Yellow
    Write-Host "Claim one with Zadig first (docs/SETUP.md step 2)." -ForegroundColor Yellow
}

Write-Host "Target $Addr, mode $Mode" -ForegroundColor Cyan
Write-Host "Put the headphones in pairing mode if this is the first run." -ForegroundColor Gray
& $Exe --addr $Addr --mode $Mode

# Returns a WinUSB-claimed radio to Windows (restores normal Bluetooth).
#   .\restore-bluetooth.ps1 -InstanceId 'USB\VID_8087&PID_0032\8&F2CB6FA&0&14'
# Use .\verify-winusb.ps1 to find the InstanceId.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstanceId
)

$ErrorActionPreference = 'Stop'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "Run this from an elevated PowerShell." }

$dev = Get-PnpDevice -InstanceId $InstanceId -ErrorAction SilentlyContinue
if (-not $dev) { throw "Device not found: $InstanceId" }

$svc = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
Write-Host "Device : $($dev.FriendlyName)"
Write-Host "Service: $svc"
if ($svc -notmatch '^winusb$') {
    Write-Host "This device is not bound to WinUSB - nothing to restore." -ForegroundColor Yellow
    return
}

Write-Host "Removing the device and its driver binding..." -ForegroundColor Yellow
try {
    Remove-PnpDevice -InstanceId $InstanceId -Confirm:$false -ErrorAction Stop
} catch {
    Write-Host "Remove-PnpDevice failed ($($_.Exception.Message)); trying pnputil." -ForegroundColor Yellow
    & pnputil /remove-device "$InstanceId"
}

Start-Sleep -Seconds 2
Write-Host "Rescanning so Windows reinstalls its own driver..." -ForegroundColor Yellow
& pnputil /scan-devices | Out-Null

Start-Sleep -Seconds 3
$now = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
if ($now -match 'bthusb') {
    Write-Host "Restored: Windows Bluetooth driver (BTHUSB) is bound again." -ForegroundColor Green
    Write-Host "Re-pair your headphones with Windows - the stored key belongs to the other stack." -ForegroundColor Gray
} else {
    Write-Host "Current service: '$now'. If this is not BTHUSB, reboot and check Device Manager." -ForegroundColor Yellow
}

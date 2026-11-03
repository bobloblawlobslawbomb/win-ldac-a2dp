# Reports which driver is bound to each Bluetooth radio, so you can tell whether
# a radio is claimed by WinUSB (this project) or still owned by Windows (BTHUSB).
#   .\verify-winusb.ps1
$ProgressPreference = 'SilentlyContinue'

$rows = foreach ($d in (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue)) {
    $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    if ($svc -in @('WinUSB','winusb','BTHUSB','bthusb','BthA2dp','BthA2dpDrv')) {
        [pscustomobject]@{
            FriendlyName = $d.FriendlyName
            Service      = $svc
            State        = if ($svc -match 'winusb') { 'CLAIMED by WinUSB (this project)' } else { 'owned by Windows' }
            InstanceId   = $d.InstanceId
        }
    }
}

if (-not $rows) { Write-Host "No Bluetooth radios found." -ForegroundColor Yellow; return }
$rows | Sort-Object State | Format-Table -AutoSize -Wrap

$claimed = @($rows | Where-Object { $_.State -like 'CLAIMED*' })
if ($claimed) {
    Write-Host "$($claimed.Count) radio(s) claimed by WinUSB. Windows Bluetooth is OFF." -ForegroundColor Yellow
    Write-Host "Restore with: .\restore-bluetooth.ps1 -InstanceId '<InstanceId>'" -ForegroundColor Gray
} else {
    Write-Host "No radio is claimed by WinUSB; Windows Bluetooth is normal." -ForegroundColor Green
    Write-Host "Claim one with Zadig before running a2dp_source_demo.exe." -ForegroundColor Gray
}

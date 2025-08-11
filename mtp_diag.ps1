param(
    [string]$InstanceId = "",
    [string]$USBVid = "0483",
    [string]$USBPid = "5740",
    [int]$DurationSec = 30
)

function New-OutputFolder {
    $ts = Get-Date -Format "yyyyMMdd_HHmmss"
    $global:OutDir = "D:\MTP_Diag_$ts"
    New-Item -Path $global:OutDir -ItemType Directory -Force | Out-Null
    return $global:OutDir
}

function Write-Info([string]$msg) { Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Write-Warn([string]$msg) { Write-Warning $msg }
function Write-Err([string]$msg)  { Write-Error $msg }

# 1) Prepare output folder & transcript
$Out = New-OutputFolder
$Transcript = Join-Path $Out "Transcript.txt"
Start-Transcript -Path $Transcript -Force | Out-Null

Write-Info "Output folder: $Out"

# 2) Resolve target device InstanceId (prefer explicit parameter)
if (-not $InstanceId -or $InstanceId.Trim() -eq "") {
    Write-Info "No InstanceId provided. Searching by VID/PID (VID_$USBVid, PID_$USBPid) or WPD class..."
    $re = "VID_$USBVid&PID_$USBPid"
    $candidates = Get-PnpDevice | Where-Object {
        $_.InstanceId -match $re -or $_.Class -eq 'WPD'
    }
    # Prefer WPD with our VID/PID
    $pick = $candidates | Where-Object { $_.InstanceId -match $re -and $_.Class -eq 'WPD' } | Select-Object -First 1
    if (-not $pick) { $pick = $candidates | Where-Object { $_.InstanceId -match $re } | Select-Object -First 1 }
    if (-not $pick) { $pick = $candidates | Where-Object { $_.Class -eq 'WPD' } | Select-Object -First 1 }
    if ($pick) { $InstanceId = $pick.InstanceId }
}

if (-not $InstanceId -or $InstanceId.Trim() -eq "") {
    Write-Warn "Could not auto-detect InstanceId. You can rerun with -InstanceId 'USB\VID_XXXX&PID_YYYY\...'"
} else {
    Write-Info "Target InstanceId = $InstanceId"
}

# 3) Save current device inventory
try {
    Get-PnpDevice | Sort-Object Class, FriendlyName |
      Select-Object Status, Class, InstanceId, FriendlyName |
      Export-Csv -Path (Join-Path $Out "PnpDevices.csv") -NoTypeInformation -Encoding UTF8
} catch { Write-Warn "Failed to export PnP devices: $_" }

try {
    Get-PnpDevice -PresentOnly | Where-Object { $_.Class -in @('WPD','USB') -or $_.FriendlyName -match 'MTP|Portable' } |
      Select-Object Status, Class, InstanceId, FriendlyName |
      Format-Table -Auto | Out-String | Set-Content -Path (Join-Path $Out "Present_USB_WPD.txt") -Encoding UTF8
} catch { Write-Warn "Failed to export present USB/WPD devices: $_" }

if ($InstanceId) {
    try {
        Get-PnpDevice -InstanceId $InstanceId | Format-List * |
          Out-String | Set-Content -Path (Join-Path $Out "Target_Device_Full.txt") -Encoding UTF8
    } catch { Write-Warn "Failed to query target device details: $_" }
}

# 4) Export recent System events for PnP/MTP (last 2 hours)
try {
    $startTime = (Get-Date).AddHours(-2)
    $prov = 'Kernel-PnP|WPD-MTPClassDriver|Microsoft-Windows-UserPnp|Microsoft-Windows-USB-USBPORT|Microsoft-Windows-USB-UCX'
    $events = Get-WinEvent -LogName System -ErrorAction Stop | Where-Object {
        $_.TimeCreated -ge $startTime -and $_.ProviderName -match $prov
    }
    if ($InstanceId) {
        $events = $events | Where-Object { $_.Message -match [regex]::Escape($InstanceId) }
    }
    $events | Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message |
      Export-Csv -Path (Join-Path $Out "SystemEvents_Filtered.csv") -NoTypeInformation -Encoding UTF8
} catch { Write-Warn "Failed to export System events: $_" }

# 5) Create ETW trace to capture MTP/USB activity
function Start-MtpTrace {
    param([string]$OutPath)
    Write-Info "Starting ETW trace for MTP/USB -> $OutPath"
    # Clean previous session if exists
    logman stop MTPDiag -ets *> $null 2>&1
    logman delete MTPDiag     *> $null 2>&1

    $cmd = @(
        'create', 'trace', 'MTPDiag',
        '-p', 'Microsoft-Windows-WPD-MTPClassDriver', '0xFFFFFFFF', '0xFF',
        '-p', 'Microsoft-Windows-UserPnp',            '0xFFFFFFFF', '0xFF',
        '-p', 'Microsoft-Windows-USB-USBPORT',        '0xFFFFFFFF', '0xFF',
        '-p', 'Microsoft-Windows-USB-UCX',            '0xFFFFFFFF', '0xFF',
        '-nb', '128', '640',
        '-bs', '1024',
        '-o',  $OutPath,
        '-ets'
    )
    logman @cmd | Out-Null
    logman start MTPDiag -ets | Out-Null
}

function Stop-MtpTrace {
    Write-Info "Stopping ETW trace"
    logman stop MTPDiag -ets | Out-Null
    Start-Sleep -Milliseconds 500
    logman delete MTPDiag | Out-Null
}

$etl = Join-Path $Out "MTPDiag.etl"
Start-MtpTrace -OutPath $etl

Write-Info "Reproduce the issue NOW (unplug/plug the device, open Explorer). Waiting $DurationSec seconds..."
for ($i = $DurationSec; $i -ge 1; $i--) {
    Write-Progress -Activity "Capturing ETW trace" -Status "$i s remaining" -PercentComplete ((($DurationSec-$i)/$DurationSec)*100)
    Start-Sleep -Seconds 1
}
Write-Progress -Activity "Capturing ETW trace" -Completed

Stop-MtpTrace

# 6) Pack results into ZIP
$zip = "D:\MTP_Diag_$((Get-Date).ToString('yyyyMMdd_HHmmss')).zip"
try {
    Compress-Archive -Path (Join-Path $Out '*') -DestinationPath $zip -Force
    Write-Info "Saved ZIP: $zip"
} catch {
    Write-Warn "Failed to zip results: $_"
    $zip = $null
}

Stop-Transcript | Out-Null

Write-Host ""
Write-Host "Done. Collected files in: $Out"
if ($zip) { Write-Host "ZIP archive: $zip" }

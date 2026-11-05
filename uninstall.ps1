# Removes everything install.ps1 added. Close OBS and the game first.
$ErrorActionPreference = "Stop"
$layerDir  = Join-Path $env:LOCALAPPDATA "ArchanVRCapture\layer"
$pluginDir = Join-Path $env:ProgramData "obs-studio\plugins\archan-vr-capture"
$key  = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
$json = Join-Path $layerDir "XR_APILAYER_ARCHAN_vr_capture.json"

if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw "OBS is running - close OBS first" }

# audio Sync Offsets the plugin set (own offset + video delay) -> own offset, in every scene collection
& (Join-Path $PSScriptRoot "tools\restore_audio_sync.ps1")

if (Get-ItemProperty -Path $key -Name $json -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $key -Name $json
}
if (Get-ItemProperty -Path $key -Name $json -ErrorAction SilentlyContinue) { throw "registry value still present" }
Write-Host "OpenXR layer unregistered"

foreach ($d in $layerDir, $pluginDir) {
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    if (Test-Path $d) { throw "still present: $d" }
    Write-Host "removed $d"
}
Write-Host "UNINSTALL OK (logs kept in $env:LOCALAPPDATA\ArchanVRCapture\logs)"

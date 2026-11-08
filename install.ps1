# Installs Archan VR Capture for the current user (no admin needed):
#  - OpenXR layer  -> %LOCALAPPDATA%\ArchanVRCapture\layer, registered in HKCU as an implicit layer
#  - OBS plugin    -> %ProgramData%\obs-studio\plugins\archan-vr-capture
# Every step is read back and compared. Close OBS before running.
$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build"

if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw "OBS is running - close it first." }

$layerSrcDll  = Join-Path $build "XR_APILAYER_ARCHAN_vr_capture.dll"
$layerSrcJson = Join-Path $root  "layer\XR_APILAYER_ARCHAN_vr_capture.json"
$pluginSrcDll = Join-Path $build "archan-vr-capture.dll"
$pluginSrcIni = Join-Path $root  "obs-plugin\data\locale\en-US.ini"
$openvrSrc    = Join-Path $root  "deps\openvr\bin\win64\openvr_api.dll"
$settingsSrc  = Join-Path $build "ArchanVRCaptureSettings.exe"
foreach ($f in $layerSrcDll, $layerSrcJson, $pluginSrcDll, $pluginSrcIni, $openvrSrc, $settingsSrc) { if (-not (Test-Path $f)) { throw "missing $f - run build.ps1 first" } }

# ---- remove the older installs under earlier names ("Quest VD to OBS", "VR Capture"); their logs are kept
$key = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
$oldInstalls = @(
    @{ Layer = "QuestVDtoOBS\layer"; Plugin = "quest-vd-obs"; Json = "XR_APILAYER_QUESTVD_obs_capture.json" },
    @{ Layer = "VRCapture\layer";    Plugin = "vr-capture";   Json = "XR_APILAYER_VRCAPTURE_obs.json" }
)
foreach ($o in $oldInstalls) {
    $oldLayerDir  = Join-Path $env:LOCALAPPDATA $o.Layer
    $oldPluginDir = Join-Path $env:ProgramData ("obs-studio\plugins\" + $o.Plugin)
    $oldJson = Join-Path $oldLayerDir $o.Json
    if (Get-ItemProperty -Path $key -Name $oldJson -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $key -Name $oldJson
    }
    if (Get-ItemProperty -Path $key -Name $oldJson -ErrorAction SilentlyContinue) { throw "old registry value still present: $oldJson" }
    Write-Host "old layer registration absent: $oldJson"
    foreach ($d in $oldLayerDir, $oldPluginDir) {
        if (Test-Path $d) { try { Remove-Item -Recurse -Force $d -ErrorAction Stop } catch {} }
        if (Test-Path $d) {
            # a running game still has the old (now unregistered) DLL loaded; run install again later
            Write-Warning "old folder still present (file in use, run install.ps1 again later): $d"
        } else {
            Write-Host "old folder absent: $d"
        }
    }
}

$layerDir  = Join-Path $env:LOCALAPPDATA "ArchanVRCapture\layer"
$pluginDir = Join-Path $env:ProgramData "obs-studio\plugins\archan-vr-capture"
New-Item -ItemType Directory -Force $layerDir | Out-Null
New-Item -ItemType Directory -Force (Join-Path $pluginDir "bin\64bit") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $pluginDir "data\locale") | Out-Null

$copies = @(
    @($layerSrcDll,  (Join-Path $layerDir "XR_APILAYER_ARCHAN_vr_capture.dll")),
    @($layerSrcJson, (Join-Path $layerDir "XR_APILAYER_ARCHAN_vr_capture.json")),
    @($pluginSrcDll, (Join-Path $pluginDir "bin\64bit\archan-vr-capture.dll")),
    @($openvrSrc,    (Join-Path $pluginDir "bin\64bit\openvr_api.dll")),
    @($pluginSrcIni, (Join-Path $pluginDir "data\locale\en-US.ini")),
    @($settingsSrc,  (Join-Path $env:LOCALAPPDATA "ArchanVRCapture\ArchanVRCaptureSettings.exe"))
)
foreach ($c in $copies) {
    $a = (Get-FileHash $c[0]).Hash
    if ((Test-Path $c[1]) -and ((Get-FileHash $c[1]).Hash -eq $a)) {
        # already identical: no copy (a running game may have the layer DLL loaded)
        Write-Host "already identical (hash verified): $($c[1])  $a"
        continue
    }
    Copy-Item -Force $c[0] $c[1]
    $b = (Get-FileHash $c[1]).Hash
    if ($a -ne $b) { throw "copy verify failed: $($c[1])" }
    Write-Host "copied + hash verified: $($c[1])  $b"
}

$json = Join-Path $layerDir "XR_APILAYER_ARCHAN_vr_capture.json"
if (-not (Test-Path $key)) { New-Item -Force $key | Out-Null }
New-ItemProperty -Path $key -Name $json -Value 0 -PropertyType DWord -Force | Out-Null
$back = (Get-ItemProperty -Path $key -Name $json).$json
if ($back -ne 0) { throw "registry verify failed ($back)" }
Write-Host "registered OpenXR layer (HKCU, enabled = 0): $json"

# Start menu shortcut to the settings window (as the installer makes it)
$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) "Archan VR Capture Settings.lnk"
$target = Join-Path $env:LOCALAPPDATA "ArchanVRCapture\ArchanVRCaptureSettings.exe"
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = $target
$s.Save()
if ($sh.CreateShortcut($lnk).TargetPath -ne $target) { throw "shortcut verify failed: $lnk" }
Write-Host "Start menu shortcut: $lnk -> $target"
Write-Host "INSTALL OK"

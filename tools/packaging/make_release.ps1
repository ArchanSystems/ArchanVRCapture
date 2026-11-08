<#
.SYNOPSIS
    Builds one signed Archan VR Capture installer from ONE version string.

.DESCRIPTION
    1. build      build.ps1 (unless -SkipBuild)
    2. test       every proof test (unless -SkipTests); any failure stops the release
    3. stage      an explicit per-file list into tools\packaging\stage -- never a glob
    4. sign       our two binaries in the stage (layer + OBS plugin), each signature proven
    5. hash       payload.manifest.txt: SHA256 and size of every staged file
    6. installer  ISCC with the Sign Tool "archan" (Setup, the uninstaller and Setup's
                  temporary copies are signed by ISCC through sign_release.ps1)
    7. verify     the finished installer; one that fails is renamed .UNSIGNED

    Signing is the same system as Archan XR: tools\packaging\sign_release.ps1 is Archan XR's
    script (Microsoft Artifact Signing, signer Archan Systems AS, SHA-256, RFC 3161 timestamp;
    needs `az login`, D:\ArchanXR\signing\metadata.json and the local, git-ignored
    signing_login.local.json with the Azure tenant and subscription).
    Valve's openvr_api.dll is third party and is never re-signed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\packaging\make_release.ps1 -Version 0.1.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Version,
    [switch] $SkipBuild,
    [switch] $SkipTests,
    [string] $Iscc = '',
    # Where the built installer lands: one folder for every build, off the development drive.
    [string] $Dist = '',
    [string] $ObsBin = 'C:\Program Files\obs-studio\bin\64bit'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string] $message) {
    Write-Host "RELEASE FAILED: $message" -ForegroundColor Red
    exit 1
}

if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { Fail "version '$Version' is not numeric (x.y.z): Windows' file version needs that" }

$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$build = Join-Path $root 'build'
$stage = Join-Path $here 'stage'
$signer = Join-Path $here 'sign_release.ps1'
if ([string]::IsNullOrEmpty($Iscc)) { $Iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe' }
if ([string]::IsNullOrEmpty($Dist)) { $Dist = $env:ARCHANVRCAPTURE_DIST }
if ([string]::IsNullOrEmpty($Dist)) { $Dist = 'E:\ArchanVRCapture_Release_Installer' }
if (-not (Test-Path -LiteralPath $Iscc)) { Fail "Inno Setup's compiler was not found at $Iscc (pass -Iscc)" }
New-Item -ItemType Directory -Force $Dist | Out-Null

# ---- 1. build -----------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host '== build' -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -NoProfile -File (Join-Path $root 'build.ps1') -ObsBin $ObsBin
    if ($LASTEXITCODE -ne 0) { Fail 'build failed' }
}

# ---- 2. tests -----------------------------------------------------------------------------
function Run-Test([string] $label, [string] $exe, [string[]] $arguments, [string] $workDir) {
    Write-Host "   test $label" -NoNewline
    $sp = @{ FilePath = $exe; WorkingDirectory = $workDir; NoNewWindow = $true; Wait = $true; PassThru = $true
        RedirectStandardOutput = (Join-Path $env:TEMP "archanvrc_test_out.txt"); RedirectStandardError = (Join-Path $env:TEMP "archanvrc_test_err.txt") }
    if ($arguments.Count -gt 0) { $sp.ArgumentList = $arguments }
    $p = Start-Process @sp
    $last = (Get-Content (Join-Path $env:TEMP "archanvrc_test_out.txt") -Tail 1)
    if ($p.ExitCode -ne 0) {
        Write-Host ''
        Get-Content (Join-Path $env:TEMP "archanvrc_test_out.txt") -Tail 15 | ForEach-Object { Write-Host "     $_" }
        Fail "test '$label' failed (exit $($p.ExitCode))"
    }
    Write-Host "  -> $last" -ForegroundColor Green
}
if (-not $SkipTests) {
    Write-Host '== tests' -ForegroundColor Cyan
    $layerDll = Join-Path $build 'XR_APILAYER_ARCHAN_vr_capture.dll'
    $mock = Join-Path $build 'layer_mock_test.exe'
    Run-Test 'stabilizer' (Join-Path $build 'stab_test.exe') @() $root
    Run-Test 'crop' $mock @('--crop') $root
    Run-Test 'log cleanup' $mock @('--cleanup') $root
    Run-Test 'log zip' $mock @('--logzip') $root
    Run-Test 'layer D3D11' $mock @("`"$layerDll`"") $root
    Run-Test 'layer D3D12' $mock @("`"$layerDll`"", '--d3d12') $root
    Run-Test 'layer quad views D3D11' $mock @("`"$layerDll`"", '--quad') $root
    Run-Test 'layer quad views D3D12' $mock @("`"$layerDll`"", '--d3d12', '--quad') $root
    Run-Test 'layer right eye quad D3D11' $mock @("`"$layerDll`"", '--quad', '--right') $root
    Run-Test 'layer right eye quad D3D12' $mock @("`"$layerDll`"", '--d3d12', '--quad', '--right') $root
    Run-Test 'overlay D3D11' $mock @("`"$layerDll`"", '--overlay') $root
    Run-Test 'overlay D3D12' $mock @("`"$layerDll`"", '--overlay', '--d3d12') $root
    Run-Test 'settings window' $mock @('--settings-app', "`"$(Join-Path $build 'ArchanVRCaptureSettings.exe')`"") $root
    Run-Test 'plugin in libobs' (Join-Path $build 'plugin_render_test.exe') @("`"$(Join-Path $build 'archan-vr-capture.dll')`"", "`"$(Join-Path $root 'obs-plugin\data')`"") $ObsBin
}

# ---- 3. stage: an explicit list, nothing else ----------------------------------------------
Write-Host '== stage' -ForegroundColor Cyan
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$files = @(
    @{ From = 'build\XR_APILAYER_ARCHAN_vr_capture.dll';      To = 'layer\XR_APILAYER_ARCHAN_vr_capture.dll'; Sign = $true },
    @{ From = 'layer\XR_APILAYER_ARCHAN_vr_capture.json';     To = 'layer\XR_APILAYER_ARCHAN_vr_capture.json' },
    @{ From = 'build\ArchanVRCaptureSettings.exe';            To = 'ArchanVRCaptureSettings.exe'; Sign = $true },
    @{ From = 'build\archan-vr-capture.dll';                  To = 'obs-plugin\bin\64bit\archan-vr-capture.dll'; Sign = $true },
    @{ From = 'build\openvr_api.dll';                         To = 'obs-plugin\bin\64bit\openvr_api.dll' },
    @{ From = 'obs-plugin\data\locale\en-US.ini';             To = 'obs-plugin\data\locale\en-US.ini' },
    @{ From = 'tools\restore_audio_sync.ps1';                 To = 'tools\restore_audio_sync.ps1' },
    @{ From = 'deps\openvr\LICENSE';                          To = 'third_party\OpenVR-LICENSE.txt' }
)
foreach ($f in $files) {
    $src = Join-Path $root $f.From
    $dst = Join-Path $stage $f.To
    if (-not (Test-Path -LiteralPath $src)) { Fail "missing $src" }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst
    if ((Get-FileHash -LiteralPath $src).Hash -ne (Get-FileHash -LiteralPath $dst).Hash) { Fail "copy differs: $dst" }
    Write-Host "   $($f.To)"
}

# ---- 4. sign our own binaries in the stage, each signature proven -------------------------
Write-Host '== sign' -ForegroundColor Cyan
$toSign = @($files | Where-Object { $_.ContainsKey('Sign') -and $_.Sign } | ForEach-Object { Join-Path $stage $_.To })
& powershell -ExecutionPolicy Bypass -NoProfile -File $signer @toSign
if ($LASTEXITCODE -ne 0) { Fail 'signing the binaries failed; nothing was packaged' }

# ---- 5. payload manifest -------------------------------------------------------------------
$manifest = Join-Path $stage 'payload.manifest.txt'
$lines = @("Archan VR Capture $Version -- staged $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')", 'SHA256  size  file')
foreach ($f in $files) {
    $p = Join-Path $stage $f.To
    $lines += '{0}  {1}  {2}' -f (Get-FileHash -LiteralPath $p).Hash, (Get-Item -LiteralPath $p).Length, $f.To
}
[IO.File]::WriteAllLines($manifest, $lines, (New-Object System.Text.UTF8Encoding($false)))

# ---- 6. the installer, signed by ISCC through the same script ------------------------------
Write-Host '== installer' -ForegroundColor Cyan
$signTool = 'archan=powershell.exe -ExecutionPolicy Bypass -NoProfile -File $q' + $signer + '$q $f'
& $Iscc "/DMyAppVersion=$Version" "/DMyOutputDir=$Dist" "/S$signTool" (Join-Path $here 'ArchanVRCapture.iss')
$isccExit = $LASTEXITCODE

# ---- 7. never an unsigned installer under a publishable name -------------------------------
$installer = Join-Path $Dist "ArchanVRCapture-$Version-Setup.exe"
$verifyExit = 1
if (Test-Path -LiteralPath $installer) {
    & powershell -ExecutionPolicy Bypass -NoProfile -File $signer -VerifyOnly $installer
    $verifyExit = $LASTEXITCODE
    if ($verifyExit -ne 0) {
        Move-Item -LiteralPath $installer -Destination "$installer.UNSIGNED" -Force
        Write-Host "The installer is not validly signed; it was renamed to $installer.UNSIGNED" -ForegroundColor Red
    }
}
if ($isccExit -ne 0) { Fail "ISCC failed ($isccExit)" }
if (-not (Test-Path -LiteralPath $installer)) { Fail "expected a signed $installer, which is not there" }
if ($verifyExit -ne 0) { exit 1 }

Write-Host ''
Write-Host "Built Archan VR Capture $Version" -ForegroundColor Green
Write-Host "  $installer"
Write-Host "  SHA256 $((Get-FileHash -LiteralPath $installer).Hash)"

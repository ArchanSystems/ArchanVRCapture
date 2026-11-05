# Builds the OpenXR layer, the OBS plugin and the proof tests (Release, x64).
# Needs: Visual Studio 18 (C++), CMake. Import libs are made from the installed OBS DLLs.
param(
    [string]$ObsBin = "C:\Program Files\obs-studio\bin\64bit"
)
$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build"
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

New-Item -ItemType Directory -Force (Join-Path $build "implibs") | Out-Null

# Import libraries for obs.dll and obs-frontend-api.dll (exports read with dumpbin)
$implibCmds = @()
foreach ($dll in "obs", "obs-frontend-api") {
    $dllPath = Join-Path $ObsBin "$dll.dll"
    if (-not (Test-Path $dllPath)) { throw "missing $dllPath" }
    $def = Join-Path $build "implibs\$dll.def"
    $lib = Join-Path $build "implibs\$dll.lib"
    $implibCmds += "dumpbin /nologo /exports `"$dllPath`" > `"$def.txt`""
    $implibCmds += "powershell -NoProfile -ExecutionPolicy Bypass -File `"$root\tools\make_def.ps1`" `"$def.txt`" `"$def`" $dll"
    $implibCmds += "lib /nologo /def:`"$def`" /machine:x64 /out:`"$lib`""
}

$cmd = @(
    "call `"$vcvars`" >nul"
) + $implibCmds + @(
    "cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release",
    "cmake --build `"$build`" --config Release"
)
$bat = Join-Path $build "do_build.bat"
Set-Content -Path $bat -Value (($cmd | ForEach-Object { "$_ || exit /b 1" }) -join "`r`n") -Encoding ascii
cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "BUILD OK -> $build"

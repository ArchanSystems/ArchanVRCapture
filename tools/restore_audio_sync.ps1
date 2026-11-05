# Puts back the audio Sync Offsets the plugin changed (own offset + video delay -> own offset) in
# OBS scene collections, for when the plugin is uninstalled while a collection still holds them.
# The plugin saves its record in each collection: modules.archan_vr_capture_audio_sync =
# [{uuid, name, own, applied}] (nanoseconds). A source is changed only if its saved "sync" is still
# exactly 'applied'. Only those numbers are edited in the file text; a backup is written first.
param(
    [string]$ScenesDir = (Join-Path $env:APPDATA "obs-studio\basic\scenes")
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $ScenesDir)) { Write-Host "no OBS scene collections in $ScenesDir"; return }
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($file in Get-ChildItem -Path $ScenesDir -Filter *.json -File) {
    $text = [IO.File]::ReadAllText($file.FullName, $utf8)
    $data = $text | ConvertFrom-Json
    $rec = $null
    if ($data.modules) { $rec = $data.modules.archan_vr_capture_audio_sync }
    if (-not $rec -or @($rec).Count -eq 0) { Write-Host "$($file.Name): no audio sync record"; continue }
    $changed = 0
    foreach ($r in @($rec)) {
        $u = [regex]::Escape([string]$r.uuid)
        $m = [regex]::Match($text, '(?m)^([ \t]*)"uuid": "' + $u + '",?\r?$')
        if (-not $m.Success) { Write-Host "$($file.Name): '$($r.name)' not found"; continue }
        $indent = [regex]::Escape($m.Groups[1].Value)
        $rest = $text.Substring($m.Index + $m.Length)
        $s = [regex]::Match($rest, '(?m)^' + $indent + '"sync": (-?\d+)')
        $nextUuid = [regex]::Match($rest, '(?m)^' + $indent + '"uuid": ')
        if (-not $s.Success -or ($nextUuid.Success -and $nextUuid.Index -lt $s.Index)) {
            Write-Host "$($file.Name): '$($r.name)' has no sync value"; continue
        }
        $cur = [int64]$s.Groups[1].Value
        if ($cur -ne [int64]$r.applied) {
            Write-Host ("{0}: '{1}' is {2} ms, not the {3} ms the plugin set - left as it is" -f $file.Name, $r.name, ($cur / 1e6), ([int64]$r.applied / 1e6))
            continue
        }
        $at = $m.Index + $m.Length + $s.Groups[1].Index
        $text = $text.Substring(0, $at) + ([int64]$r.own).ToString() + $text.Substring($at + $s.Groups[1].Length)
        Write-Host ("{0}: '{1}' {2} ms -> {3} ms (own offset back)" -f $file.Name, $r.name, ($cur / 1e6), ([int64]$r.own / 1e6))
        $changed++
    }
    if ($changed -gt 0) {
        $bak = $file.FullName + ".archan-audio-sync.bak"
        Copy-Item -LiteralPath $file.FullName -Destination $bak -Force
        [IO.File]::WriteAllText($file.FullName, $text, $utf8)
        $null = [IO.File]::ReadAllText($file.FullName, $utf8) | ConvertFrom-Json # still valid JSON
        Write-Host "$($file.Name): $changed offset(s) restored (backup $bak)"
    }
}

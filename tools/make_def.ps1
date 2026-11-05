# Turns "dumpbin /exports" output into a .def file.
param([string]$DumpFile, [string]$DefFile, [string]$LibName)
$names = New-Object System.Collections.Generic.List[string]
$inTable = $false
foreach ($line in Get-Content $DumpFile) {
    if ($line -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
    if ($inTable) {
        if ($line -match '^\s*Summary') { break }
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $names.Add($Matches[1]) }
        elseif ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+(\S+)\s+\(forwarded') { $names.Add($Matches[1]) }
    }
}
if ($names.Count -eq 0) { throw "no exports parsed from $DumpFile" }
$out = @("LIBRARY $LibName", "EXPORTS") + ($names | ForEach-Object { "    $_" })
Set-Content -Path $DefFile -Value $out -Encoding ascii
Write-Host "$LibName : $($names.Count) exports"

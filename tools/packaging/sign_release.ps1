<#
.SYNOPSIS
    Signs files with Microsoft Artifact Signing and PROVES every signature,
    or fails. Nothing unsigned leaves this script with exit code 0.

.DESCRIPTION
    The one place a release signs anything. It is called:

      stage_release.ps1   the three ArchanXR binaries in the stage, before
                          they are hashed into payload.manifest.txt
      ISCC                Sign Tool "archanxr", defined on ISCC's command
                          line by make_release.ps1: Setup, the uninstaller
                          and Setup's temporary self-copies, one file a call
      make_release.ps1    -VerifyOnly, on the finished installer

    Signing is Microsoft's own SignTool integration, never a local PFX:

      signtool sign /fd SHA256 /tr <RFC 3161 timestamp server> /td SHA256
                    /dlib Azure.CodeSigning.Dlib.dll /dmdf metadata.json

    The certificate profile, account and endpoint are in metadata.json;
    authentication is the Azure CLI login (`az login`).

    **Every file is then proven, and every proof must pass:**
      signtool verify /pa /tw    Authenticode policy, trusted chain,
                                 timestamped -- exit code 0 only
      Get-AuthenticodeSignature  Valid, and the signer is $Publisher
      the PE's own signature     file digest SHA-256, and an RFC 3161
                                 timestamp token whose digest is SHA-256

.PARAMETER Path
    The files to sign (or, with -VerifyOnly, to check).

.PARAMETER VerifyOnly
    Check the signatures only; sign nothing.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)]
    [string[]] $Path,
    [switch] $VerifyOnly,
    [string] $SignTool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe',
    # Empty means the first of the two known install locations that exists:
    # the documented one, then the per-user one the Artifact Signing Client
    # Tools installer used on the development PC (2026-11-04).
    [string] $Dlib = '',
    [string] $Metadata = 'D:\ArchanXR\signing\metadata.json',
    [string] $TimestampUrl = 'http://timestamp.acs.microsoft.com',
    [string] $Publisher = 'Archan Systems AS',
    # The Azure CLI login signing requires: Archan Systems AS's tenant and the
    # subscription holding the ArchanXR signing account. They are not in this
    # public repo: empty means they are read from $LoginFile, a local file git
    # ignores: { "tenant": "<tenant id>", "subscription": "<subscription id>" }
    [string] $ExpectedTenant = '',
    [string] $ExpectedSubscription = '',
    # Empty means signing_login.local.json next to this script ($PSScriptRoot
    # is empty in Windows PowerShell 5.1 parameter defaults, so it is set below).
    [string] $LoginFile = '',
    # **Per signing operation, and per Azure CLI check.** With no login the
    # signing dlib waited at "Submitting digest for signing..." with no end
    # (2026-11-04), so the release hung instead of failing. On expiry the
    # process and its children are ended and this script fails.
    # ARCHANXR_SIGN_TIMEOUT_SECONDS sets it for the whole release.
    [int] $TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $PSBoundParameters.ContainsKey('TimeoutSeconds') -and $env:ARCHANXR_SIGN_TIMEOUT_SECONDS) {
    $TimeoutSeconds = [int]$env:ARCHANXR_SIGN_TIMEOUT_SECONDS
}

function Fail([string] $message) {
    Write-Host "SIGNING FAILED: $message" -ForegroundColor Red
    exit 1
}

# Runs one program, at most $TimeoutSeconds. On expiry it ends that process
# and its own children (taskkill /T on its PID, nothing else). -Capture keeps
# the output for the caller instead of letting it reach the log.
function Invoke-Bounded([string] $file, [string] $arguments, [switch] $Capture) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $file
    $info.Arguments = $arguments
    $info.UseShellExecute = $false
    if ($Capture) { $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true }
    $process = [System.Diagnostics.Process]::Start($info)
    if ($Capture) { $out = $process.StandardOutput.ReadToEndAsync(); $err = $process.StandardError.ReadToEndAsync() }
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        & taskkill.exe /PID $process.Id /T /F | Out-Null
        [void]$process.WaitForExit(10000)
        return [pscustomobject]@{ TimedOut = $true; ExitCode = -1; Out = ''; Err = '' }
    }
    $process.WaitForExit()
    $result = [pscustomobject]@{ TimedOut = $false; ExitCode = $process.ExitCode; Out = ''; Err = '' }
    if ($Capture) { $result.Out = $out.Result; $result.Err = $err.Result }
    return $result
}

$SHA256 = '2.16.840.1.101.3.4.2.1'
# szOID_RFC3161_counterSign: the RFC 3161 token signtool /tr embeds as an
# unsigned attribute of the signer. A legacy Authenticode timestamp would be
# a countersignature (1.2.840.113549.1.9.6) instead, and is refused.
$RFC3161_TOKEN = '1.3.6.1.4.1.311.3.3.1'

if (-not (Test-Path -LiteralPath $SignTool)) { Fail "no signtool.exe at $SignTool" }

if (-not $VerifyOnly) {
    if ([string]::IsNullOrEmpty($Dlib)) {
        foreach ($candidate in @(
                'C:\Program Files (x86)\Microsoft\ArtifactSigningClientTools\bin\Azure.CodeSigning.Dlib.dll'
                (Join-Path $env:LOCALAPPDATA 'Microsoft\MicrosoftArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll'))) {
            if (Test-Path -LiteralPath $candidate) { $Dlib = $candidate; break }
        }
    }
    if ([string]::IsNullOrEmpty($Dlib) -or -not (Test-Path -LiteralPath $Dlib)) {
        Fail 'no Azure.CodeSigning.Dlib.dll found -- install the Artifact Signing Client Tools or pass -Dlib'
    }
    if (-not (Test-Path -LiteralPath $Metadata)) { Fail "no signing metadata at $Metadata" }
    # The dlib authenticates through `az`, found on PATH. A process started
    # before the Azure CLI was installed carries a PATH without it (seen on
    # the development PC, 2026-11-04), so its install directory is added.
    if (-not (Get-Command az -ErrorAction SilentlyContinue)) {
        $azDir = 'C:\Program Files\Microsoft SDKs\Azure\CLI2\wbin'
        if (Test-Path -LiteralPath (Join-Path $azDir 'az.cmd')) { $env:Path = "$azDir;$env:Path" }
    }

    # **The login is checked BEFORE SignTool starts**: the right tenant and
    # subscription, and a token the signing service accepts can be issued. A
    # missing or expired login fails here, with no signing request sent.
    if ([string]::IsNullOrEmpty($ExpectedTenant) -or [string]::IsNullOrEmpty($ExpectedSubscription)) {
        if ([string]::IsNullOrEmpty($LoginFile)) { $LoginFile = Join-Path $PSScriptRoot 'signing_login.local.json' }
        if (-not (Test-Path -LiteralPath $LoginFile)) {
            Fail "no -ExpectedTenant/-ExpectedSubscription and no $LoginFile -- create it: { `"tenant`": `"<id>`", `"subscription`": `"<id>`" }"
        }
        $login = Get-Content -LiteralPath $LoginFile -Raw | ConvertFrom-Json
        if ([string]::IsNullOrEmpty($ExpectedTenant)) { $ExpectedTenant = [string]$login.tenant }
        if ([string]::IsNullOrEmpty($ExpectedSubscription)) { $ExpectedSubscription = [string]$login.subscription }
        if ([string]::IsNullOrEmpty($ExpectedTenant) -or [string]::IsNullOrEmpty($ExpectedSubscription)) {
            Fail "$LoginFile needs both `"tenant`" and `"subscription`""
        }
    }
    $az = Get-Command az -ErrorAction SilentlyContinue
    if ($null -eq $az) { Fail 'the Azure CLI (az) was not found -- install it and run az login' }
    $loginHelp = "run: az login --tenant $ExpectedTenant, then az account set --subscription $ExpectedSubscription"
    $account = Invoke-Bounded $env:ComSpec ('/d /s /c ""{0}" account show --query "{{tenant:tenantId,subscription:id,user:user.name}}" -o json"' -f $az.Source) -Capture
    if ($account.TimedOut) { Fail "az account show did not answer within $TimeoutSeconds s; it was ended" }
    if ($account.ExitCode -ne 0) { Fail "Azure CLI is not logged in ($($account.Err.Trim())) -- $loginHelp" }
    $session = $account.Out | ConvertFrom-Json
    if ($session.tenant -ne $ExpectedTenant) { Fail "Azure CLI is logged in to tenant $($session.tenant), not $ExpectedTenant -- $loginHelp" }
    if ($session.subscription -ne $ExpectedSubscription) { Fail "Azure CLI's subscription is $($session.subscription), not $ExpectedSubscription -- $loginHelp" }
    $token = Invoke-Bounded $env:ComSpec ('/d /s /c ""{0}" account get-access-token --resource https://codesigning.azure.net --tenant {1} --query expiresOn -o tsv"' -f $az.Source, $ExpectedTenant) -Capture
    if ($token.TimedOut) { Fail "az account get-access-token did not answer within $TimeoutSeconds s; it was ended" }
    if ($token.ExitCode -ne 0) { Fail "Azure CLI cannot issue a signing token ($($token.Err.Trim())) -- $loginHelp" }
    Write-Host "  azure     $($session.user), tenant $($session.tenant), subscription $($session.subscription), token until $($token.Out.Trim())"
}

Add-Type -AssemblyName System.Security

# The PKCS#7 SignedData in the PE's security directory (data directory 4,
# whose address is a FILE offset), decoded.
function Get-PeSignedCms([string] $file) {
    $bytes = [System.IO.File]::ReadAllBytes($file)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $null }
    $peAt = [System.BitConverter]::ToInt32($bytes, 0x3C)
    if ([System.BitConverter]::ToUInt32($bytes, $peAt) -ne 0x00004550) { return $null }
    $optional = $peAt + 24
    $magic = [System.BitConverter]::ToUInt16($bytes, $optional)
    $dataDir = if ($magic -eq 0x20B) { $optional + 112 } else { $optional + 96 }
    $certAt = [System.BitConverter]::ToUInt32($bytes, $dataDir + 32)
    $certSize = [System.BitConverter]::ToUInt32($bytes, $dataDir + 36)
    if ($certAt -eq 0 -or $certSize -lt 8) { return $null }
    $length = [System.BitConverter]::ToUInt32($bytes, $certAt)
    $type = [System.BitConverter]::ToUInt16($bytes, $certAt + 6)
    if ($type -ne 2) { return $null }   # WIN_CERT_TYPE_PKCS_SIGNED_DATA
    $blob = New-Object byte[] ($length - 8)
    [System.Array]::Copy($bytes, [int]$certAt + 8, $blob, 0, $blob.Length)
    $cms = New-Object System.Security.Cryptography.Pkcs.SignedCms
    $cms.Decode($blob)
    return $cms
}

function Assert-Signed([string] $file) {
    $name = Split-Path -Leaf $file

    $check = Invoke-Bounded $SignTool ('verify /pa /tw "{0}"' -f $file) -Capture
    if ($check.TimedOut) { Fail "$name -- signtool verify did not finish within $TimeoutSeconds s; it was ended" }
    if ($check.ExitCode -ne 0) { Fail "$name -- signtool verify /pa /tw returned $($check.ExitCode): $($check.Err.Trim())" }

    $sig = Get-AuthenticodeSignature -LiteralPath $file
    if ($sig.Status -ne 'Valid') { Fail "$name -- signature $($sig.Status): $($sig.StatusMessage)" }
    $signer = $sig.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($signer -ne $Publisher) { Fail "$name -- signed by '$signer', not '$Publisher'" }
    if ($null -eq $sig.TimeStamperCertificate) { Fail "$name -- no timestamp" }

    $cms = Get-PeSignedCms $file
    if ($null -eq $cms) { Fail "$name -- no Authenticode signature in the file" }
    $info = $cms.SignerInfos[0]
    if ($info.DigestAlgorithm.Value -ne $SHA256) { Fail "$name -- file digest is $($info.DigestAlgorithm.FriendlyName), not SHA-256" }
    $token = $null
    foreach ($attribute in $info.UnsignedAttributes) {
        if ($attribute.Oid.Value -eq $RFC3161_TOKEN) { $token = $attribute; break }
    }
    if ($null -eq $token) { Fail "$name -- the timestamp is not RFC 3161" }
    $tst = New-Object System.Security.Cryptography.Pkcs.SignedCms
    $tst.Decode($token.Values[0].RawData)
    if ($tst.SignerInfos[0].DigestAlgorithm.Value -ne $SHA256) {
        Fail "$name -- timestamp digest is $($tst.SignerInfos[0].DigestAlgorithm.FriendlyName), not SHA-256"
    }

    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    Write-Host ("  VERIFIED  {0}  signer '{1}' {2}, SHA-256, RFC 3161 timestamp by '{3}', file SHA256 {4}" -f `
        $name, $signer, $sig.SignerCertificate.Thumbprint,
        $sig.TimeStamperCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false),
        $hash) -ForegroundColor Green
}

foreach ($file in $Path) {
    if (-not (Test-Path -LiteralPath $file)) { Fail "no file at $file" }
    $full = (Resolve-Path -LiteralPath $file).Path
    if (-not $VerifyOnly) {
        Write-Host "  signing   $full" -ForegroundColor Cyan
        $run = Invoke-Bounded $SignTool ('sign /v /fd SHA256 /tr "{0}" /td SHA256 /dlib "{1}" /dmdf "{2}" "{3}"' -f $TimestampUrl, $Dlib, $Metadata, $full)
        if ($run.TimedOut) { Fail "$(Split-Path -Leaf $full) -- signtool sign did not finish within $TimeoutSeconds s; it and its child processes were ended" }
        if ($run.ExitCode -ne 0) { Fail "$(Split-Path -Leaf $full) -- signtool sign returned $($run.ExitCode)" }
    }
    Assert-Signed $full
}
exit 0

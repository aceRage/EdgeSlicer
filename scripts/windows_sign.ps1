<#
.SYNOPSIS
  Authenticode-signs EdgeSlicer's Windows PE files with Azure Artifact Signing (signtool + dlib).

.DESCRIPTION
  OFF unless EDGESLICER_SIGN_WINDOWS=1: without it the script prints one line and exits 0 having
  touched nothing, so the NSIS hooks, local cpack runs and unconfigured CI keep working.

  When enabled it:
    - collects PE files from -Path (folders recursively by extension, single files of any name:
      NSIS hands the uninstaller over as a .tmp file);
    - leaves alone every file that already has a valid signature (Microsoft's VC runtime and
      WebView2Loader, or anything signed by an earlier run) - we never dual-sign or re-sign;
    - signs everything else, third-party payloads included (OCCT, freetype, GMP/MPFR, Sentry,
      go2rtc, ffmpeg, UltraNet), except FlashNetwork.dll unless -IncludeFlashNetwork or
      EDGESLICER_SIGN_FLASHNETWORK=1 (owner's licence decision pending);
    - logs each file's sha256 before signing (and to -ManifestPath as CSV), so a pinned upstream
      hash can still be checked after the bytes change;
    - signs in batches (one signtool call per batch, retried on failure), SHA256 file digest,
      RFC 3161 timestamp from http://timestamp.acs.microsoft.com with a SHA256 digest;
    - checks every file afterwards (valid, timestamped, expected signer) and exits non-zero if
      any file could not be signed. It fails closed: no partial success is reported as success.

  Environment (names only; values live in the GitHub environment "windows-signing"):
    EDGESLICER_SIGN_WINDOWS        '1' to sign at all
    ARTIFACT_SIGNING_ENDPOINT      e.g. https://eus.codesigning.azure.net (must match the account's region)
    ARTIFACT_SIGNING_ACCOUNT       Artifact Signing account name
    ARTIFACT_SIGNING_PROFILE       certificate profile name
    WINDOWS_SIGNER_SUBJECT         optional: expected certificate CN, checked after signing
    ARTIFACT_SIGNING_TOOLS_DIR     where signtool and the dlib were unpacked (default C:\Dev\tools\signing)
    SIGNTOOL_PATH / ARTIFACT_SIGNING_DLIB   optional explicit paths overriding the search
    EDGESLICER_SIGN_FLASHNETWORK   '1' to sign FlashNetwork.dll too
    ARTIFACT_SIGNING_DEBUG         '1' adds signtool /debug
  Authentication is the dlib's DefaultAzureCredential, narrowed to the Azure CLI login made by
  azure/login (OIDC) and, as a fallback, EnvironmentCredential (AZURE_CLIENT_ID/TENANT_ID/SECRET).

  Windows PowerShell 5.1 compatible; invoke with
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\windows_sign.ps1 -Path <folder|file> [...]

.PARAMETER TestCertificateThumbprint
  Local testing only (default $env:EDGESLICER_SIGN_TEST_THUMBPRINT, so the NSIS hooks can be
  exercised too): sign with this certificate from Cert:\CurrentUser\My instead of Artifact
  Signing (no dlib, no Azure). The post-sign check then accepts an untrusted (self-signed) chain
  as long as the signer is that certificate. CI never sets it, and the workflow's final
  verification would reject such a signature anyway.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string[]]$Path,
  [switch]$IncludeFlashNetwork,
  [switch]$Force,
  [string]$ManifestPath,
  [ValidateRange(1, 200)][int]$BatchSize = 25,
  [ValidateRange(1, 10)][int]$Attempts = 3,
  [string]$TestCertificateThumbprint = $env:EDGESLICER_SIGN_TEST_THUMBPRINT,
  [switch]$NoTimestamp
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Several values may also arrive as one ';'-separated string: `powershell -File` cannot pass arrays.
$Path = @($Path | ForEach-Object { $_ -split ';' } | Where-Object { $_ -and $_.Trim() })

if ($env:EDGESLICER_SIGN_WINDOWS -ne '1') {
  Write-Host "windows_sign: EDGESLICER_SIGN_WINDOWS is not '1' - signing is off, nothing was changed."
  exit 0
}

$TimestampUrl = 'http://timestamp.acs.microsoft.com'
$PeExtensions = @('.exe', '.dll', '.sys', '.ocx', '.cpl', '.scr', '.pyd', '.node', '.efi', '.drv', '.ax')
$testMode = -not [string]::IsNullOrWhiteSpace($TestCertificateThumbprint)
if ($NoTimestamp -and -not $testMode) { Write-Host "ERROR: -NoTimestamp is only allowed with -TestCertificateThumbprint"; exit 2 }
$signFlashNetwork = $IncludeFlashNetwork -or ($env:EDGESLICER_SIGN_FLASHNETWORK -eq '1')

function Fail([string]$Message) {
  Write-Host "::error::windows_sign: $Message"
  exit 1
}

function Test-PeFile([string]$File) {
  $fs = $null
  try {
    $fs = [System.IO.File]::Open($File, 'Open', 'Read', 'ReadWrite')
    if ($fs.Length -lt 0x40) { return $false }
    $buf = New-Object byte[] 0x40
    [void]$fs.Read($buf, 0, 0x40)
    if ($buf[0] -ne 0x4D -or $buf[1] -ne 0x5A) { return $false }
    $peOff = [BitConverter]::ToInt32($buf, 0x3C)
    if ($peOff -lt 0x40 -or ($peOff + 4) -gt $fs.Length) { return $false }
    [void]$fs.Seek($peOff, 'Begin')
    $sig = New-Object byte[] 4
    [void]$fs.Read($sig, 0, 4)
    return ($sig[0] -eq 0x50 -and $sig[1] -eq 0x45 -and $sig[2] -eq 0 -and $sig[3] -eq 0)
  } catch {
    return $false
  } finally {
    if ($fs) { $fs.Dispose() }
  }
}

function Test-IsMicrosoft($Cert) { return ($Cert -and $Cert.Subject -match '(^|,\s*)O=Microsoft Corporation(,|$)') }

function Test-SubjectMatch($Cert, [string]$Expected) {
  if ([string]::IsNullOrWhiteSpace($Expected)) { return $true }
  $e = $Expected.Trim()
  if ($Cert.Subject -eq $e) { return $true }
  if ($Cert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) -eq $e) { return $true }
  if ($e -match '=') {
    foreach ($part in ($e -split ',\s*')) { if ($Cert.Subject -notlike "*$part*") { return $false } }
    return $true
  }
  return $false
}

# Is this file signed the way we need it after our signtool call?
function Test-SignedByUs([string]$File, $TestCert) {
  $sig = Get-AuthenticodeSignature -LiteralPath $File
  $cert = $sig.SignerCertificate
  if (-not $cert) { return "not signed ($($sig.Status))" }
  if ($TestCert) {
    if ($cert.Thumbprint -ne $TestCert.Thumbprint) { return "signed by $($cert.Subject), not the test certificate" }
    if ([string]$sig.Status -notin @('Valid', 'UnknownError', 'NotTrusted')) { return "status $($sig.Status)" }
  } else {
    if ([string]$sig.Status -ne 'Valid') { return "status $($sig.Status): $($sig.StatusMessage)" }
    if (-not (Test-SubjectMatch $cert $env:WINDOWS_SIGNER_SUBJECT)) { return "signer '$($cert.Subject)' does not match WINDOWS_SIGNER_SUBJECT" }
  }
  if (-not $NoTimestamp -and -not $sig.TimeStamperCertificate) { return 'no timestamp' }
  return $null
}

function Find-Tool([string]$Override, [string]$Root, [string]$Leaf, [string]$ParentDirName) {
  if (-not [string]::IsNullOrWhiteSpace($Override)) {
    if (Test-Path -LiteralPath $Override) { return (Resolve-Path -LiteralPath $Override).ProviderPath }
    Fail "$Override does not exist"
  }
  if (-not (Test-Path -LiteralPath $Root)) { return $null }
  # Newest copy under an x64 directory (the NuGet layouts are bin\<sdkver>\x64\signtool.exe and
  # bin\x64\Azure.CodeSigning.Dlib.dll). Sorting the full path puts the highest SDK version last.
  $hit = Get-ChildItem -LiteralPath $Root -Recurse -File -Filter $Leaf -ErrorAction SilentlyContinue |
         Where-Object { $_.Directory.Name -ieq $ParentDirName } |
         Sort-Object FullName | Select-Object -Last 1
  if ($hit) { return $hit.FullName }
  return $null
}

# ---- what to sign ---------------------------------------------------------------------------
$candidates = New-Object System.Collections.ArrayList
foreach ($p in $Path) {
  if (-not (Test-Path -LiteralPath $p)) { Fail "no such path: $p" }
  $item = Get-Item -LiteralPath $p -Force
  if ($item.PSIsContainer) {
    Get-ChildItem -LiteralPath $item.FullName -Recurse -File -Force |
      Where-Object { $PeExtensions -contains $_.Extension.ToLowerInvariant() } |
      ForEach-Object { if (Test-PeFile $_.FullName) { [void]$candidates.Add($_.FullName) } }
  } else {
    if (-not (Test-PeFile $item.FullName)) { Fail "not a PE file: $($item.FullName)" }
    [void]$candidates.Add($item.FullName)
  }
}
$candidates = @($candidates | Sort-Object -Unique)
if ($candidates.Count -eq 0) { Fail "no PE files found under: $($Path -join ', ')" }

$toSign = New-Object System.Collections.ArrayList
$report = New-Object System.Collections.ArrayList
$manifest = New-Object System.Collections.ArrayList
foreach ($f in $candidates) {
  $name = [System.IO.Path]::GetFileName($f)
  $sig = Get-AuthenticodeSignature -LiteralPath $f
  $status = [string]$sig.Status
  $cert = $sig.SignerCertificate
  if ($status -eq 'Valid') {
    $who = if (Test-IsMicrosoft $cert) { 'Microsoft' } else { $cert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) }
    [void]$report.Add("skip  already signed ($who)  $f")
    continue
  }
  if ($testMode -and $cert -and $cert.Thumbprint -eq $TestCertificateThumbprint.Trim().ToUpperInvariant()) {
    [void]$report.Add("skip  already signed (test certificate)  $f")
    continue
  }
  if ($name -ieq 'FlashNetwork.dll' -and -not $signFlashNetwork) {
    [void]$report.Add("skip  FlashNetwork.dll left unsigned (EDGESLICER_SIGN_FLASHNETWORK not 1)  $f")
    continue
  }
  if ($status -ne 'NotSigned' -and -not $Force) {
    # A broken or untrusted signature (HashMismatch = modified after signing). Do not paper
    # over it by re-signing unless asked to.
    Fail "$f has a signature with status $status ($($sig.StatusMessage)); refusing to re-sign without -Force"
  }
  $sha = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant()
  [void]$manifest.Add((New-Object PSObject -Property ([ordered]@{ File = $f; Sha256BeforeSigning = $sha })))
  [void]$report.Add("sign  sha256 $sha  $f")
  [void]$toSign.Add($f)
}
$report | ForEach-Object { Write-Host $_ }
if ($ManifestPath) { $manifest | Export-Csv -LiteralPath $ManifestPath -NoTypeInformation -Encoding ascii }

if ($toSign.Count -eq 0) {
  Write-Host "windows_sign: nothing to sign ($($candidates.Count) PE files, all already signed or skipped)"
  exit 0
}

# ---- tools and metadata ---------------------------------------------------------------------
$toolsDir = if ($env:ARTIFACT_SIGNING_TOOLS_DIR) { $env:ARTIFACT_SIGNING_TOOLS_DIR } else { 'C:\Dev\tools\signing' }
$signtool = Find-Tool $env:SIGNTOOL_PATH $toolsDir 'signtool.exe' 'x64'
if (-not $signtool -and $testMode) {
  $signtool = Find-Tool '' "${env:ProgramFiles(x86)}\Windows Kits\10\bin" 'signtool.exe' 'x64'
}
if (-not $signtool) { Fail "signtool.exe not found under $toolsDir (set SIGNTOOL_PATH or ARTIFACT_SIGNING_TOOLS_DIR)" }
Write-Host "signtool: $signtool"

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("edgeslicer_sign_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work -Force | Out-Null

$testCert = $null
$baseArgs = @('sign', '/v', '/fd', 'SHA256')
if (-not $NoTimestamp) { $baseArgs += @('/tr', $TimestampUrl, '/td', 'SHA256') }
if ($env:ARTIFACT_SIGNING_DEBUG -eq '1') { $baseArgs += '/debug' }

if ($testMode) {
  $testCert = Get-Item -LiteralPath ("Cert:\CurrentUser\My\" + $TestCertificateThumbprint.Trim()) -ErrorAction SilentlyContinue
  if (-not $testCert) { Fail "test certificate $TestCertificateThumbprint not found in Cert:\CurrentUser\My" }
  Write-Host "TEST MODE: signing with $($testCert.Subject) from CurrentUser\My, not Artifact Signing"
  $baseArgs += @('/sha1', $testCert.Thumbprint, '/s', 'My')
} else {
  $dlib = Find-Tool $env:ARTIFACT_SIGNING_DLIB $toolsDir 'Azure.CodeSigning.Dlib.dll' 'x64'
  if (-not $dlib) { Fail "Azure.CodeSigning.Dlib.dll not found under $toolsDir (set ARTIFACT_SIGNING_DLIB or ARTIFACT_SIGNING_TOOLS_DIR)" }
  Write-Host "dlib:     $dlib"
  $missing = @('ARTIFACT_SIGNING_ENDPOINT', 'ARTIFACT_SIGNING_ACCOUNT', 'ARTIFACT_SIGNING_PROFILE') |
             Where-Object { [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($_)) }
  if ($missing) { Fail "signing is on but $($missing -join ', ') is not set" }
  if ($env:ARTIFACT_SIGNING_ENDPOINT -notmatch '^https://[a-z0-9]+\.codesigning\.azure\.net/?$') {
    Fail "ARTIFACT_SIGNING_ENDPOINT does not look like https://<region>.codesigning.azure.net"
  }
  $meta = [ordered]@{
    Endpoint               = $env:ARTIFACT_SIGNING_ENDPOINT.Trim()
    CodeSigningAccountName = $env:ARTIFACT_SIGNING_ACCOUNT.Trim()
    CertificateProfileName = $env:ARTIFACT_SIGNING_PROFILE.Trim()
    # Only the credentials this pipeline uses: the Azure CLI session from azure/login (OIDC)
    # and the client-secret fallback. Skipping the rest avoids e.g. a managed-identity probe
    # timing out on the VM before the real credential is tried.
    ExcludeCredentials     = @('ManagedIdentityCredential', 'WorkloadIdentityCredential', 'SharedTokenCacheCredential',
                               'VisualStudioCredential', 'VisualStudioCodeCredential', 'AzurePowerShellCredential',
                               'AzureDeveloperCliCredential', 'InteractiveBrowserCredential')
  }
  if ($env:GITHUB_RUN_ID) { $meta['CorrelationId'] = "github-run-$($env:GITHUB_RUN_ID)" }
  $metaPath = Join-Path $work 'metadata.json'
  [System.IO.File]::WriteAllText($metaPath, ($meta | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
  $baseArgs += @('/dlib', $dlib, '/dmdf', $metaPath)
}

# ---- sign -----------------------------------------------------------------------------------
try {
  $batches = New-Object System.Collections.ArrayList
  $cur = New-Object System.Collections.ArrayList; $curLen = 0
  foreach ($f in $toSign) {
    # Keep each command line far below Windows' 32767-character limit.
    if ($cur.Count -ge $BatchSize -or ($curLen + $f.Length + 3) -gt 24000) {
      [void]$batches.Add(@($cur)); $cur = New-Object System.Collections.ArrayList; $curLen = 0
    }
    [void]$cur.Add($f); $curLen += $f.Length + 3
  }
  if ($cur.Count) { [void]$batches.Add(@($cur)) }

  $n = 0
  foreach ($batch in $batches) {
    $n++
    $pending = @($batch)
    for ($attempt = 1; $attempt -le $Attempts -and $pending.Count; $attempt++) {
      Write-Host ("--- batch {0}/{1}, attempt {2}: {3} file(s)" -f $n, $batches.Count, $attempt, $pending.Count)
      $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
      & $signtool @baseArgs @pending 2>&1 | ForEach-Object { Write-Host "    $_" }
      $rc = $LASTEXITCODE
      $ErrorActionPreference = $prev
      # Whatever signtool says, the files decide: keep only those not yet properly signed.
      $pending = @($pending | Where-Object { $null -ne (Test-SignedByUs $_ $testCert) })
      if ($pending.Count -eq 0) { break }
      Write-Host "signtool exited $rc; $($pending.Count) file(s) still unsigned"
      if ($attempt -lt $Attempts) { Start-Sleep -Seconds (10 * $attempt) }
    }
  }
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

# ---- final check ----------------------------------------------------------------------------
$bad = New-Object System.Collections.ArrayList
foreach ($f in $toSign) {
  $why = Test-SignedByUs $f $testCert
  if ($null -ne $why) { [void]$bad.Add("$f : $why") }
}
if ($bad.Count) {
  $bad | ForEach-Object { Write-Host "NOT SIGNED  $_" }
  Fail "$($bad.Count) of $($toSign.Count) file(s) are not properly signed"
}
Write-Host "windows_sign: signed $($toSign.Count) file(s); $($candidates.Count - $toSign.Count) left as they were"
exit 0

<#
.SYNOPSIS
  Checks that every Windows PE file (exe, dll, ...) in a folder, zip or installer carries a valid,
  timestamped Authenticode signature from the expected publisher (or from Microsoft).

.DESCRIPTION
  Used by the self-hosted Windows workflow after signing, by the CPack staging hook, and by the
  release publisher before `gh release create`. Read-only: it never modifies what it checks
  (a zip or an extracted installer is unpacked into a fresh temporary folder, deleted afterwards).

  A file passes when ALL of these hold:
    - Get-AuthenticodeSignature reports Status = Valid;
    - the signature is embedded in the file (a catalog signature does not travel with the file);
    - it has an RFC 3161 / Authenticode timestamp (Artifact Signing certificates live 72 hours,
      so an untimestamped signature dies three days after the build);
    - the signer is Microsoft (O=Microsoft Corporation: the VC runtime, WebView2Loader) or, when
      -ExpectedSubject / WINDOWS_SIGNER_SUBJECT is set, matches it. When neither is set, any
      valid signer is accepted and the table says so.

  Exit codes: 0 every file passed (or was explicitly allowed), 1 at least one file failed,
  2 bad arguments (a path that does not exist, nothing to check).

  Windows PowerShell 5.1 compatible. Run it like the workflow does:
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\windows_verify_signatures.ps1 -Path <folder|zip|exe> [...]

.PARAMETER Path
  One or more folders, .zip files or single files (the installer, an exe, a dll). Folders are
  scanned recursively; zips are extracted to a temporary folder first.

.PARAMETER ExpectedSubject
  The publisher's certificate CN (or a full subject string). Defaults to $env:WINDOWS_SIGNER_SUBJECT.

.PARAMETER AllowUnsigned
  File-name or relative-path wildcards that may be unsigned (reported as ALLOWED, never fail the
  run), e.g. 'FlashNetwork.dll' while the owner's licence decision is pending.

.PARAMETER ExtractInstaller
  Also unpack each NSIS installer given in -Path with 7-Zip ($env:SEVENZIP, else
  C:\Program Files\7-Zip\7z.exe) and check the PE files inside.

.PARAMETER AllFiles
  Detect PE files by their MZ/PE header across every file instead of by extension. Slower on a
  tree with thousands of resource files; the default extension list already covers what we ship.

.PARAMETER ShowAuthenticodeHash
  Adds the file's Authenticode SHA256 (the digest a signature covers). It is identical for an
  unsigned payload and the signed copy of it, so it can confirm that a signed DLL is the same
  code as a pinned unsigned one (whose plain sha256 changes when it is signed).

.PARAMETER TestCertificateThumbprint
  Local testing only (default $env:EDGESLICER_SIGN_TEST_THUMBPRINT, matching windows_sign.ps1):
  also accept files signed by this (self-signed, untrusted) certificate. Reported as OK-TEST and
  announced as TEST MODE. The workflow refuses to run with that variable set.

.PARAMETER HashOnly
  Print only "<authenticode sha256>  <relative path>" for each PE file (sha256sum style) and exit
  0 without judging signatures. For a publisher that pins a payload's hash: the line for a
  signed copy equals the line for the unsigned original.

.PARAMETER CsvPath
  Also write the result table to this CSV file.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string[]]$Path,
  [string]$ExpectedSubject = $env:WINDOWS_SIGNER_SUBJECT,
  [string[]]$AllowUnsigned = @(),
  [switch]$ExtractInstaller,
  [switch]$AllFiles,
  [switch]$ShowAuthenticodeHash,
  [switch]$HashOnly,
  [string]$CsvPath,
  [string]$TestCertificateThumbprint = $env:EDGESLICER_SIGN_TEST_THUMBPRINT
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Several values may also arrive as one ';'-separated string: `powershell -File` cannot pass arrays.
$Path = @($Path | ForEach-Object { $_ -split ';' } | Where-Object { $_ -and $_.Trim() })
$AllowUnsigned = @($AllowUnsigned | ForEach-Object { $_ -split ';' } | Where-Object { $_ -and $_.Trim() })

# Extensions Windows maps as images. Anything else is only caught with -AllFiles.
$PeExtensions = @('.exe', '.dll', '.sys', '.ocx', '.cpl', '.scr', '.pyd', '.node', '.efi', '.drv', '.ax', '.tmp')

function Test-PeFile([string]$File) {
  # MZ header, then 'PE\0\0' at e_lfanew. Cheap: reads at most the first 4 KB.
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

function Get-PeFiles([string]$Root, [bool]$Deep) {
  $files = Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction Stop
  if (-not $Deep) { $files = $files | Where-Object { $PeExtensions -contains $_.Extension.ToLowerInvariant() } }
  foreach ($f in $files) { if (Test-PeFile $f.FullName) { $f } }
}

# Authenticode digest (SHA256) per the PE/COFF Authenticode spec: the whole file except the
# CheckSum field, the Certificate Table directory entry and the certificate table itself. An
# unsigned file is padded (virtually) with zeros to an 8-byte boundary, which is what signtool
# writes before appending the signature, so the digest of a payload and of its signed copy match.
function Get-AuthenticodeSha256([string]$File) {
  $fs = [System.IO.File]::Open($File, 'Open', 'Read', 'ReadWrite')
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $len = $fs.Length
    $br = New-Object System.IO.BinaryReader($fs)
    [void]$fs.Seek(0x3C, 'Begin'); $peOff = $br.ReadInt32()
    $opt = $peOff + 24
    [void]$fs.Seek($opt, 'Begin'); $magic = $br.ReadUInt16()
    $csumOff = $opt + 64
    $dirBase = if ($magic -eq 0x20B) { $opt + 112 } else { $opt + 96 }
    $certDirOff = $dirBase + 4 * 8
    [void]$fs.Seek($certDirOff, 'Begin')
    $certOff = [int64]$br.ReadUInt32(); $certSize = [int64]$br.ReadUInt32()
    $hasCert = ($certSize -gt 0 -and $certOff -gt 0 -and ($certOff + $certSize) -le $len)
    $end = if ($hasCert) { $certOff } else { $len }

    $chunk = New-Object byte[] 1048576
    $ranges = @(
      @(0, $csumOff),
      @(($csumOff + 4), $certDirOff),
      @(($certDirOff + 8), $end)
    )
    if ($hasCert -and ($certOff + $certSize) -lt $len) { $ranges += , @(($certOff + $certSize), $len) }
    foreach ($r in $ranges) {
      $pos = [int64]$r[0]; $stop = [int64]$r[1]
      [void]$fs.Seek($pos, 'Begin')
      while ($pos -lt $stop) {
        $want = [int][Math]::Min([int64]$chunk.Length, $stop - $pos)
        $got = $fs.Read($chunk, 0, $want)
        if ($got -le 0) { throw "unexpected end of file in $File" }
        [void]$sha.TransformBlock($chunk, 0, $got, $null, 0)
        $pos += $got
      }
    }
    if (-not $hasCert -and ($len % 8) -ne 0) {
      $pad = New-Object byte[] (8 - ($len % 8))
      [void]$sha.TransformBlock($pad, 0, $pad.Length, $null, 0)
    }
    [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
    return (($sha.Hash | ForEach-Object { $_.ToString('x2') }) -join '')
  } finally {
    $sha.Dispose(); $fs.Dispose()
  }
}

# True when the file carries its own certificate table (an embedded signature). Windows may
# validate a file through a system catalog instead (Get-AuthenticodeSignature then reports
# SignatureType Catalog even for a file that is also embedded-signed); a catalog does not travel
# with the file to a user's machine, so only the embedded table counts here.
function Test-HasEmbeddedSignature([string]$File) {
  $fs = [System.IO.File]::Open($File, 'Open', 'Read', 'ReadWrite')
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    [void]$fs.Seek(0x3C, 'Begin'); $peOff = $br.ReadInt32()
    $opt = $peOff + 24
    [void]$fs.Seek($opt, 'Begin'); $magic = $br.ReadUInt16()
    $dirBase = if ($magic -eq 0x20B) { $opt + 112 } else { $opt + 96 }
    [void]$fs.Seek($dirBase + 4 * 8, 'Begin')
    $certOff = [int64]$br.ReadUInt32(); $certSize = [int64]$br.ReadUInt32()
    return ($certSize -gt 0 -and $certOff -gt 0 -and ($certOff + $certSize) -le $fs.Length)
  } finally {
    $fs.Dispose()
  }
}

function Test-SubjectMatch($Cert, [string]$Expected) {
  if ([string]::IsNullOrWhiteSpace($Expected)) { return $false }
  $e = $Expected.Trim()
  if ($Cert.Subject -eq $e) { return $true }
  $cn = $Cert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
  if ($cn -eq $e) { return $true }
  # A full or partial subject string ("CN=Jane Doe, O=Jane Doe, L=..."): every RDN must appear.
  if ($e -match '=') {
    foreach ($part in ($e -split ',\s*')) { if ($Cert.Subject -notlike "*$part*") { return $false } }
    return $true
  }
  return $false
}

function Test-IsMicrosoft($Cert) {
  return ($Cert.Subject -match '(^|,\s*)O=Microsoft Corporation(,|$)')
}

function Test-Allowed([string]$Rel, [string]$Name) {
  foreach ($pat in $AllowUnsigned) {
    if ([string]::IsNullOrWhiteSpace($pat)) { continue }
    if ($Name -like $pat -or $Rel -like $pat) { return $true }
  }
  return $false
}

$sevenZip = if ($env:SEVENZIP) { $env:SEVENZIP } else { 'C:\Program Files\7-Zip\7z.exe' }
$tempDirs = New-Object System.Collections.ArrayList
$targets = New-Object System.Collections.ArrayList   # @{ File; Rel; Origin }

function Add-Tree([string]$Root, [string]$Origin) {
  $rootFull = (Resolve-Path -LiteralPath $Root).ProviderPath.TrimEnd('\')
  foreach ($f in (Get-PeFiles $rootFull ([bool]$AllFiles))) {
    $rel = $f.FullName.Substring($rootFull.Length).TrimStart('\')
    [void]$targets.Add(@{ File = $f.FullName; Rel = $rel; Origin = $Origin })
  }
}

function New-TempDir([string]$Tag) {
  $d = Join-Path ([System.IO.Path]::GetTempPath()) ("edgeslicer_verify_{0}_{1}" -f $Tag, [guid]::NewGuid().ToString('N').Substring(0, 8))
  New-Item -ItemType Directory -Path $d -Force | Out-Null
  [void]$tempDirs.Add($d)
  return $d
}

$exitCode = 0
try {
  foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host "ERROR: no such path: $p"; exit 2 }
    $item = Get-Item -LiteralPath $p -Force
    if ($item.PSIsContainer) {
      Add-Tree $item.FullName $item.Name
    } elseif ($item.Extension -ieq '.zip') {
      $d = New-TempDir 'zip'
      Write-Host "extracting $($item.FullName)"
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      [System.IO.Compression.ZipFile]::ExtractToDirectory($item.FullName, $d)
      Add-Tree $d $item.Name
    } else {
      if (-not (Test-PeFile $item.FullName)) { Write-Host "ERROR: not a PE file: $($item.FullName)"; exit 2 }
      [void]$targets.Add(@{ File = $item.FullName; Rel = $item.Name; Origin = '(file)' })
      if ($ExtractInstaller) {
        if (-not (Test-Path -LiteralPath $sevenZip)) { Write-Host "ERROR: -ExtractInstaller needs 7-Zip at $sevenZip (set SEVENZIP)"; exit 2 }
        $d = New-TempDir 'inst'
        Write-Host "extracting $($item.FullName) with 7-Zip"
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        & $sevenZip x -y "-o$d" $item.FullName 2>&1 | Out-Null
        $rc = $LASTEXITCODE; $ErrorActionPreference = $prev
        if ($rc -ne 0) { Write-Host "ERROR: 7-Zip could not extract $($item.FullName) (exit $rc)"; exit 2 }
        Add-Tree $d ("{0} (contents)" -f $item.Name)
      }
    }
  }

  if ($targets.Count -eq 0) { Write-Host "ERROR: no PE files found under: $($Path -join ', ')"; exit 2 }

  if ($HashOnly) {
    foreach ($t in $targets) { Write-Output ("{0}  {1}" -f (Get-AuthenticodeSha256 $t.File), $t.Rel) }
    exit 0
  }

  if ($TestCertificateThumbprint) {
    Write-Host "TEST MODE: files signed by test certificate $TestCertificateThumbprint are accepted (never use this for a release)"
  }
  if ([string]::IsNullOrWhiteSpace($ExpectedSubject)) {
    Write-Host "note: no -ExpectedSubject / WINDOWS_SIGNER_SUBJECT - any valid, timestamped signer is accepted"
  } else {
    Write-Host "expected signer: $ExpectedSubject (or Microsoft)"
  }

  $rows = New-Object System.Collections.ArrayList
  foreach ($t in $targets) {
    $sig = Get-AuthenticodeSignature -LiteralPath $t.File
    $cert = $sig.SignerCertificate
    $signer = if ($cert) { $cert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) } else { '' }
    $ts = [bool]$sig.TimeStamperCertificate
    $problems = @()
    $isTest = ($cert -and $TestCertificateThumbprint -and $cert.Thumbprint -eq $TestCertificateThumbprint.Trim().ToUpperInvariant() -and
               @('Valid', 'UnknownError', 'NotTrusted') -contains [string]$sig.Status)
    if ([string]$sig.Status -ne 'Valid' -and -not $isTest) { $problems += [string]$sig.Status }
    if ($cert) {
      if (-not (Test-HasEmbeddedSignature $t.File)) { $problems += 'no embedded signature (catalog only)' }
      if (-not $ts) { $problems += 'no timestamp' }
      $isMs = Test-IsMicrosoft $cert
      if (-not $isMs -and -not $isTest -and -not [string]::IsNullOrWhiteSpace($ExpectedSubject) -and -not (Test-SubjectMatch $cert $ExpectedSubject)) {
        $problems += "unexpected signer"
      }
    }
    $result = 'OK'
    if ($problems.Count) {
      $result = if (-not $cert -and (Test-Allowed $t.Rel ([System.IO.Path]::GetFileName($t.File)))) { 'ALLOWED' } else { 'FAIL' }
    }
    if ($result -eq 'OK' -and $isTest) { $result = 'OK-TEST' }
    if ($result -eq 'FAIL') { $exitCode = 1 }
    $row = [ordered]@{
      Result    = $result
      File      = $t.Rel
      Status    = [string]$sig.Status
      Signer    = $signer
      Timestamp = if ($ts) { 'yes' } else { 'no' }
      Issue     = ($problems -join '; ')
      Origin    = $t.Origin
    }
    if ($ShowAuthenticodeHash) { $row['AuthenticodeSha256'] = Get-AuthenticodeSha256 $t.File }
    [void]$rows.Add((New-Object PSObject -Property $row))
  }

  $rows | Sort-Object Origin, File | Format-Table -AutoSize -Wrap | Out-String -Width 400 | Write-Host
  if ($CsvPath) { $rows | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Encoding ascii }

  $nOk = @($rows | Where-Object { $_.Result -like 'OK*' }).Count
  $nAllowed = @($rows | Where-Object { $_.Result -eq 'ALLOWED' }).Count
  $nFail = @($rows | Where-Object { $_.Result -eq 'FAIL' }).Count
  Write-Host ("checked {0} PE files: {1} OK, {2} allowed unsigned, {3} FAILED" -f $rows.Count, $nOk, $nAllowed, $nFail)
  if ($nFail) { Write-Host "SIGNATURE CHECK FAILED" } else { Write-Host "signature check passed" }
} finally {
  foreach ($d in $tempDirs) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
}
exit $exitCode

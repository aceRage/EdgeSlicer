<#
  Closes every EdgeSlicer process that runs from the given install folder(s), so the NSIS
  installer / uninstaller can replace or remove the files. Called by the installer through
  nsExec (see cmake/nsis/SnapmakerCloseRunning.nsh); it is shipped inside the installer and
  copied next to the uninstaller, and it never needs the EdgeSlicer.exe it is closing, so it
  works on a hub from any older version.

  What counts as "ours": only processes whose ExecutablePath lies UNDER one of the folders in
  -Dirs. A test copy, a portable copy or another install is never touched. The installer's own
  process chain and any Uninstall*.exe are skipped.

  Order:
    1. A GUI with a visible window is the user's work: never killed. With -CloseGui it is asked to
       close (WM_CLOSE, so the normal "save project?" prompt appears); if it is still open
       afterwards the script reports it and stops, before touching anything else.
    2. Hub-managed hidden slicer instances are asked to quit through the hub (the hub saves an
       unsaved project under <datadir>\hub\saves), then the hub itself: POST /hub/quit on its
       loopback control plane, with the secret from <datadir>\hub\hub.json. The hub stops go2rtc
       (and so ffmpeg) itself; on Windows a kill-on-close job object does it.
    3. Whatever still runs from the folder (a hub that did not answer, go2rtc.exe, ffmpeg.exe, an
       orphaned hidden instance) is terminated by PID.

  Exit codes: 0 nothing (left) running, 3 a GUI window is open, 4 a process could not be stopped,
  1 the script itself failed (the installer then falls back to its old tasklist check).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Dirs,   # install folders, '|' separated
    [switch]$CloseGui,                             # ask visible EdgeSlicer windows to close
    [switch]$ListOnly,                             # report what would be closed, change nothing
    [int]$HubWaitSec = 10,
    [int]$GuiWaitSec = 60
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Write-Log([string]$Text) { Write-Output ("EdgeSlicer installer: " + $Text) }

# ---- the folders ---------------------------------------------------------------------------
$prefixes = New-Object System.Collections.Generic.List[string]
foreach ($d in ($Dirs -split '\|')) {
    $d = $d.Trim().Trim('"').TrimEnd('\', '/')
    if ($d.Length -eq 0) { continue }
    # A prefix that is empty, relative or a drive root would match half the machine.
    if (-not [System.IO.Path]::IsPathRooted($d) -or $d.Length -lt 4 -or $d -match '^[A-Za-z]:$' -or $d.StartsWith('\\')) {
        Write-Log ("refusing the folder '" + $d + "' (not a full install path)")
        exit 1
    }
    $prefixes.Add($d + '\')
}
if ($prefixes.Count -eq 0) { Write-Log 'no install folder given'; exit 1 }

# ---- processes -----------------------------------------------------------------------------
function Get-Scan {
    $all = @(Get-CimInstance -ClassName Win32_Process)
    $parentOf = @{}
    foreach ($p in $all) { $parentOf[[int]$p.ProcessId] = [int]$p.ParentProcessId }
    # This script, its PowerShell host and everything that started it (the installer, or the
    # uninstaller that runs from the install folder).
    $skip = New-Object 'System.Collections.Generic.HashSet[int]'
    $cur = [int]$PID
    while ($cur -gt 0 -and $skip.Add($cur) -and $parentOf.ContainsKey($cur)) { $cur = $parentOf[$cur] }
    $out = New-Object System.Collections.Generic.List[object]
    foreach ($p in $all) {
        $exe = [string]$p.ExecutablePath
        if ($exe.Length -eq 0) { continue }
        $procId = [int]$p.ProcessId
        if ($skip.Contains($procId)) { continue }
        $under = $false
        foreach ($pre in $prefixes) {
            if ($exe.StartsWith($pre, [System.StringComparison]::OrdinalIgnoreCase)) { $under = $true; break }
        }
        if (-not $under) { continue }
        $name = [System.IO.Path]::GetFileName($exe)
        if ($name -like 'Uninstall*.exe') { continue }
        $cmd = [string]$p.CommandLine
        $isApp = $name -match '^(EdgeSlicer|snapmaker-orca|Snapmaker_Orca)\.exe$'
        $isHub = $isApp -and ($cmd -match '(^|\s)--hub(\s|$)')
        $kind = if ($isHub) { 'hub' } elseif ($isApp) { 'gui' } else { 'helper' }
        $out.Add([pscustomobject]@{
            Pid = $procId; Name = $name; Exe = $exe; Cmd = $cmd; Kind = $kind
            Created = [string]$p.CreationDate
        })
    }
    return $out.ToArray()    # callers wrap the call in @(): zero results must stay zero
}

function Test-VisibleWindow([int]$ProcId) {
    $gp = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
    return ($null -ne $gp -and $gp.MainWindowHandle -ne [IntPtr]::Zero)
}

function Test-Alive([int]$ProcId) { return ($null -ne (Get-Process -Id $ProcId -ErrorAction SilentlyContinue)) }

function Wait-Gone([int[]]$Pids, [int]$Seconds, [scriptblock]$AlsoDone = $null) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $left = @($Pids | Where-Object { Test-Alive $_ })
        if ($AlsoDone) { $left = @($left | Where-Object { -not (& $AlsoDone $_) }) }
        if ($left.Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 400
    }
    $left = @($Pids | Where-Object { Test-Alive $_ })
    if ($AlsoDone) { $left = @($left | Where-Object { -not (& $AlsoDone $_) }) }
    return ($left.Count -eq 0)
}

# ---- the hub's control plane ---------------------------------------------------------------
function Invoke-Hub([string]$Method, [int]$Port, [string]$Path, [string]$Secret, [int]$TimeoutMs = 5000) {
    try {
        $req = [System.Net.HttpWebRequest]::Create('http://127.0.0.1:' + $Port + $Path)
        $req.Method = $Method
        $req.Proxy = $null
        $req.Timeout = $TimeoutMs
        $req.ReadWriteTimeout = $TimeoutMs
        $req.KeepAlive = $false
        $req.ServicePoint.Expect100Continue = $false
        $req.Headers.Add('X-Hub-Secret', $Secret)    # never logged
        if ($Method -eq 'POST') {
            $bytes = [System.Text.Encoding]::UTF8.GetBytes('{}')    # a bodyless POST hangs the hub's own client
            $req.ContentType = 'application/json'
            $req.ContentLength = $bytes.Length
            $s = $req.GetRequestStream()
            $s.Write($bytes, 0, $bytes.Length)
            $s.Close()
        }
        $resp = $req.GetResponse()
        try {
            $rd = New-Object System.IO.StreamReader($resp.GetResponseStream())
            return $rd.ReadToEnd()
        } finally { $resp.Close() }
    } catch {
        return $null
    }
}

# The control plane of the hub with this pid: hub.json of its data dir names the pid, the loopback
# port and the per-run secret. The data dir comes from the hub's own --datadir, else the default
# ones of this and every other user profile (an elevated installer may run as another account).
function Get-HubControl([object]$Hub) {
    $dirs = New-Object System.Collections.Generic.List[string]
    if ($Hub.Cmd -match '--datadir(?:\s+|=)(?:"([^"]+)"|(\S+))') {
        $dirs.Add($(if ($Matches[1]) { $Matches[1] } else { $Matches[2] }))
    }
    $roots = New-Object System.Collections.Generic.List[string]
    if ($env:APPDATA) { $roots.Add($env:APPDATA) }
    $usersDir = Split-Path -Parent $env:USERPROFILE
    if ($usersDir -and (Test-Path -LiteralPath $usersDir)) {
        foreach ($u in @(Get-ChildItem -LiteralPath $usersDir -Directory -ErrorAction SilentlyContinue)) {
            $roots.Add((Join-Path $u.FullName 'AppData\Roaming'))
        }
    }
    foreach ($root in ($roots | Select-Object -Unique)) {
        foreach ($n in 'EdgeSlicer', 'UltraOne', 'Snapmaker-Ultra', 'Snapmaker_Orca') { $dirs.Add((Join-Path $root $n)) }
    }
    foreach ($dd in $dirs) {
        $file = Join-Path (Join-Path $dd 'hub') 'hub.json'
        if (-not (Test-Path -LiteralPath $file)) { continue }
        try {
            $j = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
            if ([int]$j.pid -ne $Hub.Pid) { continue }
            $port = [int]$j.admin_port
            if ($port -le 0) { $port = [int]$j.port }    # a hub from before the control plane split
            if ($port -gt 0 -and $j.secret) { return @{ Port = $port; Secret = [string]$j.secret } }
        } catch { }
    }
    return $null
}

function Stop-Verified([object]$P) {
    # Re-check right before killing: same pid, same image, same start time (no recycled pid).
    $now = Get-CimInstance -ClassName Win32_Process -Filter ("ProcessId=" + $P.Pid) -ErrorAction SilentlyContinue
    if ($null -eq $now) { return }
    if ([string]$now.ExecutablePath -ne $P.Exe -or [string]$now.CreationDate -ne $P.Created) { return }
    Stop-Process -Id $P.Pid -Force -ErrorAction SilentlyContinue
}

# ---- main ----------------------------------------------------------------------------------
try {
    $procs = @(Get-Scan)
    if ($procs.Count -eq 0) { Write-Log 'nothing from this install is running'; exit 0 }
    foreach ($p in $procs) { Write-Log ("found " + $p.Kind + " " + $p.Name + " (pid " + $p.Pid + ")") }

    # 1. the user's own windows
    $visible = @($procs | Where-Object { $_.Kind -eq 'gui' -and (Test-VisibleWindow $_.Pid) })
    if ($ListOnly) {
        if ($visible.Count -gt 0) { exit 3 } else { exit 0 }
    }
    if ($visible.Count -gt 0) {
        if (-not $CloseGui) {
            Write-Log ("EdgeSlicer is open in a window (pid " + (($visible | ForEach-Object { $_.Pid }) -join ', ') + ")")
            exit 3
        }
        foreach ($v in $visible) {
            $gp = Get-Process -Id $v.Pid -ErrorAction SilentlyContinue
            if ($gp) { Write-Log ("asking EdgeSlicer (pid " + $v.Pid + ") to close"); [void]$gp.CloseMainWindow() }
        }
        # Gone, or closed to the hub's tray (a hub-managed window only hides): either is fine.
        $null = Wait-Gone ($visible | ForEach-Object { $_.Pid }) $GuiWaitSec { param($id) -not (Test-VisibleWindow $id) }
        $procs = @(Get-Scan)
        $visible = @($procs | Where-Object { $_.Kind -eq 'gui' -and (Test-VisibleWindow $_.Pid) })
        if ($visible.Count -gt 0) {
            Write-Log 'EdgeSlicer is still open; it was not closed'
            exit 3
        }
    }

    # 2. the hub, politely: its hidden slicer instances first, then the hub itself
    $hubs = @($procs | Where-Object { $_.Kind -eq 'hub' })
    foreach ($h in $hubs) {
        $ctl = Get-HubControl $h
        if ($null -eq $ctl) { Write-Log ("hub pid " + $h.Pid + ": no control plane found, will end it by pid"); continue }
        $ours = @($procs | ForEach-Object { $_.Pid })
        $body = Invoke-Hub 'GET' $ctl.Port '/hub/instances' $ctl.Secret
        if ($body) {
            try {
                foreach ($i in @(($body | ConvertFrom-Json).instances)) {
                    if ($i.hidden -eq $true -and $ours -contains [int]$i.pid) {
                        Write-Log ("asking hidden instance pid " + $i.pid + " to quit")
                        $null = Invoke-Hub 'POST' $ctl.Port ('/hub/instances/' + [int]$i.pid + '/quit') $ctl.Secret 15000
                    }
                }
            } catch { }
        }
    }
    $hiddenApps = @($procs | Where-Object { $_.Kind -eq 'gui' })
    if ($hiddenApps.Count -gt 0 -and $hubs.Count -gt 0) {
        $null = Wait-Gone ($hiddenApps | ForEach-Object { $_.Pid }) 8
    }
    foreach ($h in $hubs) {
        if (-not (Test-Alive $h.Pid)) { continue }
        $ctl = Get-HubControl $h
        if ($null -eq $ctl) { continue }
        Write-Log ("asking hub pid " + $h.Pid + " to quit")
        $null = Invoke-Hub 'POST' $ctl.Port '/hub/quit' $ctl.Secret
    }
    if ($hubs.Count -gt 0) {
        if (Wait-Gone ($hubs | ForEach-Object { $_.Pid }) $HubWaitSec) { Write-Log 'the hub quit' }
        else { Write-Log ("the hub did not quit within " + $HubWaitSec + " s") }
        Start-Sleep -Milliseconds 1500    # its go2rtc / ffmpeg go with it (kill-on-close job)
    }

    # 3. whatever still runs from the folder
    $left = @(Get-Scan)
    foreach ($p in $left) {
        Write-Log ("ending " + $p.Name + " (pid " + $p.Pid + ")")
        Stop-Verified $p
    }
    if ($left.Count -gt 0) {
        $null = Wait-Gone ($left | ForEach-Object { $_.Pid }) 8
        Start-Sleep -Milliseconds 500
    }
    $still = @(Get-Scan)
    if ($still.Count -gt 0) {
        foreach ($p in $still) { Write-Log ("could not stop " + $p.Name + " (pid " + $p.Pid + ")") }
        exit 4
    }
    Write-Log 'done'
    exit 0
} catch {
    Write-Log ('failed: ' + $_.Exception.Message)
    exit 1
}

<#
  Test for cmake/nsis/close_edgeslicer.ps1 (what the NSIS installer / uninstaller run to close the
  hub before files are replaced). Uses stand-ins only, in a scratch folder under %TEMP%:
    * "EdgeSlicer.exe --hub" = a copy of powershell.exe running a tiny fake hub (loopback HTTP
      control plane + hub.json, like the real one) - it exits only on an authenticated POST /hub/quit
    * "go2rtc.exe" = a copy of ping.exe
    * "EdgeSlicer.exe" with a window = a copy of powershell.exe showing an off-screen, non-activating form
  and a second set of the same in ANOTHER folder, which must never be touched.
  It starts and ends only its own processes (by pid) and removes only its own scratch folder.
  Run:  powershell -NoProfile -ExecutionPolicy Bypass -File tests\installer\test_close_edgeslicer.ps1
#>
param([string]$Closer = (Join-Path $PSScriptRoot '..\..\cmake\nsis\close_edgeslicer.ps1'))
$ErrorActionPreference = 'Stop'
$Closer = (Resolve-Path -LiteralPath $Closer).Path
$root = Join-Path $env:TEMP ('installhub_test_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $root | Out-Null
$started = New-Object System.Collections.Generic.List[int]
$failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host ("PASS  " + $what) } else { Write-Host ("FAIL  " + $what); $script:failures++ }
}
function Alive([int]$p) { return $null -ne (Get-Process -Id $p -ErrorAction SilentlyContinue) }

$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$pingExe = Join-Path $env:SystemRoot 'System32\PING.EXE'

# ---- stand-ins -------------------------------------------------------------------------------
$fakeHub = @'
param()
# args: --hub --datadir <dir> --secret <s> --log <file>
$dd = $args[$args.IndexOf('--datadir') + 1]
$secret = $args[$args.IndexOf('--secret') + 1]
$log = $args[$args.IndexOf('--log') + 1]
$l = New-Object System.Net.HttpListener
$port = 0
foreach ($try in 1..50) {
    $port = Get-Random -Minimum 20000 -Maximum 60000
    try { $l.Prefixes.Clear(); $l.Prefixes.Add("http://127.0.0.1:$port/"); $l.Start(); break } catch { $port = 0 }
}
New-Item -ItemType Directory -Force -Path (Join-Path $dd 'hub') | Out-Null
@{ pid = $PID; port = 13640; admin_port = $port; secret = $secret } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dd 'hub\hub.json')
while ($l.IsListening) {
    $c = $l.GetContext()
    $ok = $c.Request.Headers['X-Hub-Secret'] -eq $secret
    $path = $c.Request.Url.AbsolutePath
    $body = '{}'
    if (-not $ok) { $c.Response.StatusCode = 403 }
    elseif ($path -eq '/hub/instances') { $body = '{"instances":[]}' }
    elseif ($path -eq '/hub/quit' -and $c.Request.HttpMethod -eq 'POST') { $body = '{"ok":true}'; Add-Content -LiteralPath $log -Value 'quit'; $l.Stop() }
    $b = [Text.Encoding]::UTF8.GetBytes($body)
    $c.Response.OutputStream.Write($b, 0, $b.Length)
    $c.Response.Close()
}
'@
$fakeGui = @'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace W -Name N -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);'
$f = New-Object System.Windows.Forms.Form
$f.StartPosition = 'Manual'    # (ShowInTaskbar = $false would give it an owner window, so it would not be the process's main window)
$f.Location = New-Object System.Drawing.Point(-32000, -32000); $f.Size = New-Object System.Drawing.Size(120, 80); $f.Opacity = 0.01
$null = $f.Handle
[void][W.N]::ShowWindow($f.Handle, 4)   # SW_SHOWNOACTIVATE
[System.Windows.Forms.Application]::Run($f)
'@
$hubScript = Join-Path $root 'fake_hub.ps1'
$guiScript = Join-Path $root 'fake_gui.ps1'
Set-Content -LiteralPath $hubScript -Value $fakeHub -Encoding ASCII
Set-Content -LiteralPath $guiScript -Value $fakeGui -Encoding ASCII

function New-Install([string]$name) {
    $dir = Join-Path $root $name
    New-Item -ItemType Directory -Path (Join-Path $dir 'resources\tools\go2rtc') -Force | Out-Null
    Copy-Item -LiteralPath $psExe -Destination (Join-Path $dir 'EdgeSlicer.exe')
    Copy-Item -LiteralPath $pingExe -Destination (Join-Path $dir 'resources\tools\go2rtc\go2rtc.exe')
    Copy-Item -LiteralPath $pingExe -Destination (Join-Path $dir 'resources\tools\go2rtc\ffmpeg.exe')
    return $dir
}
function Start-FakeHub([string]$inst, [string]$tag) {
    $dd = Join-Path $root ('data_' + $tag)
    $log = Join-Path $root ($tag + '_quit.log')
    $secret = [guid]::NewGuid().ToString('N')
    $p = Start-Process -FilePath (Join-Path $inst 'EdgeSlicer.exe') -WindowStyle Hidden -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $hubScript + '"'), '--hub', '--datadir', ('"' + $dd + '"'), '--secret', $secret, '--log', ('"' + $log + '"'))
    $started.Add($p.Id)
    $deadline = (Get-Date).AddSeconds(20)
    while (-not (Test-Path -LiteralPath (Join-Path $dd 'hub\hub.json')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    $g = Start-Process -FilePath (Join-Path $inst 'resources\tools\go2rtc\go2rtc.exe') -WindowStyle Hidden -PassThru -ArgumentList @('-n', '600', '127.0.0.1')
    $started.Add($g.Id)
    $f = Start-Process -FilePath (Join-Path $inst 'resources\tools\go2rtc\ffmpeg.exe') -WindowStyle Hidden -PassThru -ArgumentList @('-n', '600', '127.0.0.1')
    $started.Add($f.Id)
    return @{ Hub = $p.Id; Go2rtc = $g.Id; Ffmpeg = $f.Id; Log = $log; Dd = $dd }
}
function Run-Closer([string]$dirs, [string[]]$extra = @()) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $out = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Closer -Dirs $dirs @extra 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    $out | ForEach-Object { Write-Host ("      | " + $_) }
    return $code
}

try {
    $inst1 = New-Install 'inst1'
    $inst2 = New-Install 'inst2_other_folder'
    $a = Start-FakeHub $inst1 'a'
    $b = Start-FakeHub $inst2 'b'
    Check ((Alive $a.Hub) -and (Alive $a.Go2rtc) -and (Alive $a.Ffmpeg)) 'folder 1 stand-ins are running'
    Check ((Alive $b.Hub) -and (Alive $b.Go2rtc)) 'folder 2 stand-ins are running'

    # 1. a folder with a similar prefix must not match
    $rc = Run-Closer ($inst1 + '_x') @('-ListOnly')
    Check ($rc -eq 0 -and (Alive $a.Hub)) 'a folder that only shares a name prefix matches nothing'

    # 2. dangerous folders are refused
    $rc = Run-Closer 'C:\' @('-ListOnly')
    Check ($rc -eq 1) 'a drive root is refused'
    $rc = Run-Closer '' @('-ListOnly')
    Check ($rc -ne 0) 'an empty folder list is refused'

    # 3. list only changes nothing
    $rc = Run-Closer $inst1 @('-ListOnly')
    Check ($rc -eq 0 -and (Alive $a.Hub) -and (Alive $a.Go2rtc)) '-ListOnly changes nothing'

    # 4. the real run: polite quit, then the leftovers, folder 2 untouched
    $rc = Run-Closer $inst1
    Check ($rc -eq 0) 'closer exits 0 for a hub + go2rtc + ffmpeg'
    Check ((Test-Path -LiteralPath $a.Log) -and ((Get-Content -LiteralPath $a.Log -Raw) -match 'quit')) 'the hub got an authenticated POST /hub/quit (polite path)'
    Check (-not (Alive $a.Hub)) 'the hub is gone'
    Check (-not (Alive $a.Go2rtc)) 'go2rtc.exe from the folder is gone'
    Check (-not (Alive $a.Ffmpeg)) 'ffmpeg.exe from the folder is gone'
    Check ((Alive $b.Hub) -and (Alive $b.Go2rtc) -and (Alive $b.Ffmpeg)) 'the hub and helpers of the OTHER folder are untouched'
    Check (-not (Test-Path -LiteralPath $b.Log)) 'the other folder''s hub was never asked to quit'

    # 5. a hub that cannot be found through hub.json (old/odd data dir) is ended by pid
    $c = Start-FakeHub $inst1 'c'
    Remove-Item -LiteralPath (Join-Path $c.Dd 'hub\hub.json') -Force
    $rc = Run-Closer $inst1
    Check ($rc -eq 0 -and -not (Alive $c.Hub) -and -not (Alive $c.Go2rtc)) 'a hub without a findable control plane is ended by pid'
    Check (Alive $b.Hub) 'the other folder is still untouched'

    # 6. a hub whose secret is wrong is not trusted (403): ended by pid after the wait, still gone
    $d = Start-FakeHub $inst1 'd'
    $j = Get-Content -LiteralPath (Join-Path $d.Dd 'hub\hub.json') -Raw | ConvertFrom-Json
    $j.secret = 'not-the-secret'
    $j | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $d.Dd 'hub\hub.json')
    $rc = Run-Closer $inst1 @('-HubWaitSec', '3')
    Check ($rc -eq 0 -and -not (Alive $d.Hub)) 'a hub that refuses the request is ended by pid'
    Check (-not (Test-Path -LiteralPath $d.Log)) 'and it was not told to quit with a wrong secret accepted'

    # 7. a GUI window is never killed; -CloseGui asks it to close
    # -NoNewWindow, not -WindowStyle Hidden: a hidden start makes Windows override the form's first
    # ShowWindow, and the form must be visible (off-screen, no activation) to count as a window.
    $gui = Start-Process -FilePath (Join-Path $inst1 'EdgeSlicer.exe') -NoNewWindow -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $guiScript + '"'))
    $started.Add($gui.Id)
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Process -Id $gui.Id -ErrorAction SilentlyContinue).MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    $e = Start-FakeHub $inst1 'e'
    $rc = Run-Closer $inst1 @('-ListOnly')
    Check ($rc -eq 3) 'a visible EdgeSlicer window is reported (exit 3)'
    $rc = Run-Closer $inst1
    Check ($rc -eq 3 -and (Alive $gui.Id)) 'without -CloseGui the window stays open (exit 3)'
    Check ((Alive $e.Hub) -and (Alive $e.Go2rtc)) 'and the hub is left alone while a window is open'
    $rc = Run-Closer $inst1 @('-CloseGui', '-GuiWaitSec', '20')
    Check ($rc -eq 0 -and -not (Alive $gui.Id)) '-CloseGui closes the window (WM_CLOSE) and the run completes'
    Check (-not (Alive $e.Hub) -and -not (Alive $e.Go2rtc)) 'then the hub and helpers are closed'
    Check ((Alive $b.Hub) -and (Alive $b.Go2rtc)) 'the other folder is still untouched at the end'
} finally {
    foreach ($id in $started) {
        $pr = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($pr -and $pr.Path -and $pr.Path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Milliseconds 800
    if ($root.StartsWith((Join-Path $env:TEMP 'installhub_test_'), [System.StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $root)) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if ($failures -gt 0) { Write-Host ("$failures check(s) failed"); exit 1 }
Write-Host 'all checks passed'
exit 0

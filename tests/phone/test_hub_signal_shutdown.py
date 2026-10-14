"""Loopback test: a terminate request from outside stops the phone hub the way the tray's Quit does.
(fix/hub-signal-shutdown)

Starts a test hub (`<exe> --hub --datadir <new temp dir>`; it takes the first free port from 13640
up, so next to a running hub it lands on a spare one), waits for its go2rtc, sends it ONE kind of
terminate request, and checks afterwards that:
  - the hub process exited by itself, before the shutdown watchdog would have had to step in;
  - its go2rtc (and anything else it started: crashpad_handler) is gone;
  - hub.log has "RemoteHub: shutting down (<reason>)", and no "exiting now" from the watchdog;
  - hub.json is gone and last_exit.json names the reason;
  - (POSIX) go2rtc.pid is gone.

Ways to ask (--how):
  close       Windows: WM_CLOSE posted to the hub's hidden wx window (class wxWindowNR). Not
              `taskkill /PID` without /F: that closes whichever windows of the process it picks
              (a driver's and an IME's on 2026-10-12) and never reached the hub's own
  endsession  Windows: WM_QUERYENDSESSION then WM_ENDSESSION (ENDSESSION_LOGOFF) sent to the hub's
              top-level windows - what Windows sends at sign-out, restart and shut down
  request     POST /hub/quit on the control plane (what the installer and a newer slicer use)
  sigterm     macOS / Linux: kill -TERM (what macOS sends at logout and restart)
  sigint      macOS / Linux: kill -INT
  all         every way this platform has, one fresh hub each (the default)

Only processes this script started are ever stopped, and only by PID (a leftover is reported as a
failure and then ended through the handle opened when it was found). A running hub - the owner's
on 13640 - is never touched: this hub has its own data dir and is only ever addressed by its pid
and its own control port. %APPDATA%\\EdgeSlicer is never touched.

Usage:
  python -I tests/phone/test_hub_signal_shutdown.py C:/Dev/EdgeSlicerTest/<copy>/EdgeSlicer.exe [--how all]
Exit code 0 when every expectation holds.
"""
import argparse, glob, json, os, shutil, signal, subprocess, sys, tempfile, time, urllib.request

WIN = os.name == "nt"
failures = []


def expect(what, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + what + (("  [" + str(detail) + "]") if detail else ""), flush=True)
    if not ok:
        failures.append(what)
    return ok


# ------------------------------------------------------------------------------ processes ----

if WIN:
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    u32 = ctypes.WinDLL("user32", use_last_error=True)

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    SYNCHRONIZE, PROCESS_TERMINATE, PROCESS_QUERY_LIMITED_INFORMATION = 0x00100000, 0x0001, 0x1000

    def children_of(ppid):
        """[(pid, exe name)] of the processes whose parent is `ppid`."""
        out = []
        snap = k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
        if snap in (None, wintypes.HANDLE(-1).value):
            return out
        try:
            e = PROCESSENTRY32W()
            e.dwSize = ctypes.sizeof(e)
            ok = k32.Process32FirstW(snap, ctypes.byref(e))
            while ok:
                if e.th32ParentProcessID == ppid and e.th32ProcessID != ppid:
                    out.append((int(e.th32ProcessID), e.szExeFile))
                ok = k32.Process32NextW(snap, ctypes.byref(e))
        finally:
            k32.CloseHandle(snap)
        return out

    class Proc:
        """A process found while the hub ran, held by handle so a reused pid is never mistaken for it."""

        def __init__(self, pid, name):
            self.pid, self.name = pid, name
            self.h = k32.OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)

        def alive(self):
            return bool(self.h) and k32.WaitForSingleObject(self.h, 0) == 0x102  # WAIT_TIMEOUT

        def kill(self):
            if self.alive():
                k32.TerminateProcess(self.h, 1)

        def close(self):
            if self.h:
                k32.CloseHandle(self.h)
                self.h = None

    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    u32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
    u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    u32.SendMessageTimeoutW.restype = ctypes.c_ssize_t
    u32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM, wintypes.UINT,
                                        wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]

    def top_windows(pid):
        found = []

        def cb(hwnd, _):
            p = wintypes.DWORD()
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid:
                found.append(hwnd)
            return True

        u32.EnumWindows(WNDENUMPROC(cb), 0)
        return found

    def class_name(hwnd):
        buf = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(hwnd, buf, 256)
        return buf.value
else:
    def children_of(ppid):
        out = []
        try:
            r = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,comm="], capture_output=True, text=True, timeout=10)
            for line in r.stdout.splitlines():
                parts = line.split(None, 2)
                if len(parts) == 3 and parts[1] == str(ppid):
                    out.append((int(parts[0]), os.path.basename(parts[2])))
        except Exception:
            pass
        return out

    class Proc:
        def __init__(self, pid, name):
            self.pid, self.name = pid, name

        def alive(self):
            try:
                os.kill(self.pid, 0)
            except ProcessLookupError:
                return False
            except PermissionError:
                return True
            # A zombie is gone for our purposes.
            r = subprocess.run(["ps", "-o", "stat=", "-p", str(self.pid)], capture_output=True, text=True)
            return not r.stdout.strip().startswith("Z") and r.stdout.strip() != ""

        def kill(self):
            if self.alive():
                os.kill(self.pid, signal.SIGKILL)

        def close(self):
            pass


# -------------------------------------------------------------------------------- the test ----

def hub_log_text(dd):
    text = ""
    for f in sorted(glob.glob(os.path.join(dd, "log", "hub.log*"))):
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                text += fh.read()
        except OSError:
            pass
    return text


def one_run(exe, how):
    print("\n== %s: %s" % (how, exe), flush=True)
    dd = tempfile.mkdtemp(prefix="hubsignal_")
    hub_dir = os.path.join(dd, "hub")
    hub_json = os.path.join(hub_dir, "hub.json")
    env = dict(os.environ)
    env.pop("SNORCA_PHONE_ACCESS", None)
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if WIN else 0
    p = subprocess.Popen([exe, "--hub", "--datadir", dd], env=env, creationflags=flags,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=not WIN)
    print("hub pid", p.pid, "data dir", dd, flush=True)
    started = []  # Proc: everything the hub started that this run has seen
    ok_run = False
    try:
        hj, t0 = {}, time.time()
        while time.time() - t0 < 60 and p.poll() is None:
            try:
                with open(hub_json, encoding="utf-8") as fh:
                    hj = json.load(fh)
                if hj.get("pid") == p.pid and hj.get("admin_port") and hj.get("go2rtc_port"):
                    break
            except Exception:
                pass
            time.sleep(0.3)
        if not expect("the hub starts and runs go2rtc", hj.get("pid") == p.pid and bool(hj.get("go2rtc_port")),
                      {k: hj.get(k) for k in ("pid", "port", "admin_port", "go2rtc_port")}):
            return
        print("hub on port %s (control %s)" % (hj.get("port"), hj.get("admin_port")), flush=True)
        time.sleep(1.5)  # go2rtc up, crashpad's handler (if this build starts one) too
        kids = children_of(p.pid)
        for pid, name in kids:
            started.append(Proc(pid, name))
        go2rtc = [c for c in started if c.name.lower().startswith("go2rtc")]
        crashpad = [c for c in started if c.name.lower().startswith("crashpad_handler")]
        print("children:", ", ".join("%s %d" % (c.name, c.pid) for c in started) or "none", flush=True)
        if not expect("go2rtc runs as the hub's child", len(go2rtc) == 1 and go2rtc[0].alive()):
            return
        if not WIN:
            expect("go2rtc.pid names it", open(os.path.join(hub_dir, "go2rtc.pid")).read().strip() == str(go2rtc[0].pid))

        reason = {"close": "close", "endsession": "session_end", "request": "request", "sigterm": "signal", "sigint": "signal"}[how]
        t_ask = time.time()
        if how == "close":
            # WM_CLOSE to the hub's own wx window (the tray icon's hidden frame). Not taskkill: without
            # /F it posts WM_CLOSE to whichever of the process's windows it picks, and on 2026-10-12
            # that was a driver window (NVOpenGLPbuffer) and an IME window, never wxWindowNR.
            u32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
            frames = [w for w in top_windows(p.pid) if class_name(w) == "wxWindowNR"]
            expect("the hub has its hidden wx window", len(frames) >= 1, [class_name(w) for w in top_windows(p.pid)])
            for w in frames:
                u32.PostMessageW(w, 0x0010, 0, 0)  # WM_CLOSE
        elif how == "endsession":
            wins = top_windows(p.pid)
            print("top-level windows:", ", ".join(class_name(w) for w in wins) or "none", flush=True)
            res = ctypes.c_size_t(0)
            ENDSESSION_LOGOFF, SMTO_NORMAL = 0x80000000, 0
            answers = []
            for w in wins:  # what Windows does: ask every window first...
                ok = u32.SendMessageTimeoutW(w, 0x0011, 0, ENDSESSION_LOGOFF, SMTO_NORMAL, 5000, ctypes.byref(res))
                answers.append((class_name(w), bool(ok), res.value))
            expect("nobody objects to the session ending (WM_QUERYENDSESSION)", all(a[2] != 0 for a in answers if a[1]), answers)
            for w in wins:  # ...then tell them it ends
                u32.SendMessageTimeoutW(w, 0x0016, 1, ENDSESSION_LOGOFF, SMTO_NORMAL, 30000, ctypes.byref(res))
            print("WM_ENDSESSION handled after %.1f s" % (time.time() - t_ask), flush=True)
        elif how == "request":
            req = urllib.request.Request("http://127.0.0.1:%d/hub/quit" % hj["admin_port"], data=b"{}", method="POST",
                                         headers={"X-Hub-Secret": hj.get("secret", ""), "Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5).read()
        elif how == "sigterm":
            os.kill(p.pid, signal.SIGTERM)
        elif how == "sigint":
            os.kill(p.pid, signal.SIGINT)

        try:
            code = p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            code = None
        took = time.time() - t_ask
        expect("the hub exits by itself, well before the %d s watchdog" % 20, code is not None and took < 15,
               "exit code %s after %.1f s" % (code, took))
        # crashpad's handler notices its client is gone within a moment. A one-shot query tool is
        # different: when 13640 is taken (the owner's hub) the hub runs `netstat -ano` / `tasklist`
        # on a helper thread to name the holder, with an 8 s limit of its own; a hub that quits
        # sooner leaves it to finish by itself. Those only have to be gone within 15 s.
        lasting = [c for c in started if c.name.lower().split(".")[0] in ("go2rtc", "ffmpeg", "crashpad_handler")]
        oneshot = [c for c in started if c not in lasting]
        t_gone = time.time()
        while time.time() - t_gone < 5 and any(c.alive() for c in lasting):
            time.sleep(0.2)
        for c in lasting:
            expect("%s (pid %d) is gone after the hub" % (c.name, c.pid), not c.alive())
        while time.time() - t_gone < 15 and any(c.alive() for c in oneshot):
            time.sleep(0.5)
        for c in oneshot:
            expect("one-shot %s (pid %d) ended by itself" % (c.name, c.pid), not c.alive(), "%.1f s after the hub" % (time.time() - t_gone))
        log = hub_log_text(dd)
        expect('hub.log has "RemoteHub: shutting down (%s)"' % reason, ("RemoteHub: shutting down (%s)" % reason) in log)
        expect("the watchdog did not have to step in", "began to quit; exiting now" not in log)
        if not WIN:
            expect("go2rtc.pid is gone", not os.path.exists(os.path.join(hub_dir, "go2rtc.pid")))
            expect('hub.log says go2rtc stopped', "RemoteHub: go2rtc (pid %d) stopped" % go2rtc[0].pid in log)
        expect("hub.json is gone", not os.path.exists(hub_json))
        try:
            with open(os.path.join(hub_dir, "last_exit.json"), encoding="utf-8") as fh:
                note = json.load(fh)
        except Exception as e:
            note = {"error": str(e)}
        expect('last_exit.json says "%s"' % reason, note.get("reason") == reason and note.get("pid") == p.pid, note)
        lines = [l for l in log.splitlines() if "RemoteHub:" in l and any(k in l for k in ("received", "asked to close", "session", "shutting down", "go2rtc (pid"))]
        for l in lines[-8:]:
            print("   log:", l.strip()[-160:], flush=True)
        ok_run = True
    finally:
        # Only what this run started, by PID / handle.
        if p.poll() is None:
            print("ending the test hub (pid %d)" % p.pid, flush=True)
            p.kill()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                pass
        for c in started:
            if c.alive():
                print("ending leftover %s (pid %d)" % (c.name, c.pid), flush=True)
                c.kill()
            c.close()
        if ok_run and not failures:
            shutil.rmtree(dd, ignore_errors=True)
        else:
            print("data dir kept for a look:", dd, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--how", default="all", choices=["all", "close", "endsession", "request", "sigterm", "sigint"])
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if not os.path.isfile(exe):
        print("no such file:", exe)
        return 2
    ways = (["request", "close", "endsession"] if WIN else ["request", "sigterm", "sigint"]) if a.how == "all" else [a.how]
    for how in ways:
        if WIN and how.startswith("sig") or not WIN and how in ("close", "endsession"):
            print("\n== %s: not a request this platform has, skipped" % how)
            continue
        one_run(exe, how)
    print("\n%s: %d failure(s)%s" % ("PASS" if not failures else "FAIL", len(failures),
                                     "" if not failures else ": " + "; ".join(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

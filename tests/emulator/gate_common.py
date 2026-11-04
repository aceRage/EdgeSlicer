"""What the E2 send gates share: the fixture made with the install's CLI, the scratch data dir, the
hidden instance, the hub API, and a row/check ledger with known product gaps.

Taken from test_send_qidi_gate.py (E1), parametrised by printer model. Rules (design 5): every
printer address is asserted loopback before anything starts; nothing is written to
%APPDATA%\\EdgeSlicer or the owner's emulator home; only processes the gate started are stopped,
by PID (or, for a hub keeper left behind, by its command line naming the gate's own data dir).

Known product gaps: a row can be marked `gap="..."`. Its failures then print XFAIL and do not fail
the gate; if such a row passes, it prints XPASS and DOES fail the gate, so the marker is removed
when the product catches up.
"""
import json
import os
import shutil
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

from emu_proc import REPO, assert_loopback

DEFAULT_EXE = r"C:\Dev\EdgeSlicerBuilds\current\EdgeSlicer.exe"
DEFAULT_TEMPLATE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Temp", "snorca_hubtest", "dd_lan")
TOKEN = "testtoken12345"


def http(url, method="GET", body=None, timeout=60, headers=None):
    data = body.encode() if isinstance(body, str) else body
    req = urllib.request.Request(url, method=method, data=data, headers=headers or {})
    if body is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        raw, st = r.read(), r.status
    except urllib.error.HTTPError as e:
        raw, st = e.read(), e.code
    except Exception as e:  # noqa: BLE001
        return 0, {"error": str(e)}
    try:
        return st, json.loads(raw or b"{}")
    except ValueError:
        return st, {"raw": raw[:400].decode("utf-8", "replace")}


class Ledger:
    """Rows of checks. check() records; rows marked with a product gap turn failures into XFAIL."""

    def __init__(self, title):
        self.title = title
        self.rows = {}        # name -> {"ok": bool, "gap": str, "title": str}
        self.order = []
        self.current = None
        self.fails = []
        self.checks = 0

    def row(self, name, title, gap=""):
        self.current = name
        self.rows[name] = {"ok": True, "gap": gap, "title": title}
        self.order.append(name)
        print("\n-- %s: %s --%s" % (name, title, ("\n   (known product gap: %s)" % gap) if gap else ""), flush=True)

    def check(self, cond, what):
        self.checks += 1
        r = self.rows.get(self.current)
        tag = "ok  " if cond else ("XFAIL" if r and r["gap"] else "FAIL")
        print("  %-5s %s" % (tag, what), flush=True)
        if not cond:
            if r:
                r["ok"] = False
            self.fails.append((self.current, what))
        return cond

    def summary(self):
        """Prints the table; -> exit code (0 when every non-gap row passed and no gap row passed)."""
        bad = 0
        print("\n== %s: %d checks ==" % (self.title, self.checks))
        for name in self.order:
            r = self.rows[name]
            if r["gap"]:
                verdict = "XFAIL (known gap)" if not r["ok"] else "XPASS - the gap is closed, remove the marker"
                bad += 1 if r["ok"] else 0
            else:
                verdict = "PASS" if r["ok"] else "FAIL"
                bad += 0 if r["ok"] else 1
            print("  %-6s %-48s %s" % (name, r["title"][:48], verdict))
        for row, what in self.fails:
            if not self.rows.get(row, {}).get("gap"):
                print("  FAILED [%s]: %s" % (row, what.splitlines()[0]))
        return 1 if bad else 0


def make_fixture(exe, work, out_path, printer_preset, process_preset, filaments, colours):
    """Two 20 mm cubes, the first on filament 1, the second on filament 2, exported as a project by
    the install's own CLI on a scratch data dir (see test_send_qidi_gate.make_fixture)."""
    cube = os.path.join(REPO, "tests", "data", "20mm_cube.obj")
    raw = out_path[:-4] + "_raw.3mf"
    cmd = [exe, "--allow-newer-file", "--datadir", os.path.join(work, "clidd"), "--printer-preset", printer_preset,
           "--process-preset", process_preset, "--filament-presets", ";".join(filaments),
           "--filament-colour", ";".join(colours), "--arrange", "1", "--export-3mf", raw, cube, cube]
    log_path = os.path.join(work, "cli_fixture.log")
    with open(log_path, "w", encoding="utf-8") as log:
        rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=300,
                            cwd=work).returncode
    if rc != 0 or not os.path.isfile(raw):
        raise SystemExit("could not make the fixture (CLI exit %s, see %s)" % (rc, log_path))
    with zipfile.ZipFile(raw) as zin, zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "Metadata/model_settings.config":
                text = data.decode("utf-8")
                tag = '<metadata key="extruder" value="0"/>'
                if text.count(tag) != 2:
                    raise SystemExit("unexpected model_settings.config in the fixture (%d extruder tags)" % text.count(tag))
                text = text.replace(tag, '<metadata key="extruder" value="1"/>', 1)
                text = text.replace(tag, '<metadata key="extruder" value="2"/>', 1)
                data = text.encode("utf-8")
            zout.writestr(item, data)


def make_datadir(template, dd, fixture, model, vendor, print_host_devices=None, snapmaker_lan=None):
    """A copy of the template's presets with the owner's printers, access codes and devices taken
    out, the hub on with TOKEN, and exactly the devices given here (all loopback)."""
    if not os.path.isdir(template) or not os.path.isfile(os.path.join(template, "EdgeSlicer.conf")):
        raise SystemExit("no template data dir at %s (one whose first-run wizard is done; --template=)" % template)
    for m in (print_host_devices or {}).get("models", {}).values():
        for d in m["devices"]:
            assert_loopback(d["address"])
    for d in (snapmaker_lan or {}).get("devices", []):
        assert_loopback(d["ip"])
    os.makedirs(os.path.join(dd, "hub"), exist_ok=True)
    for name in ("EdgeSlicer.conf", "hms", "ota", "plugins", "printers", "system", "user", "web", "ultranet", "FlashNetwork"):
        src = os.path.join(template, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(dd, name))
        elif os.path.isfile(src):
            shutil.copy2(src, os.path.join(dd, name))
    p = os.path.join(dd, "EdgeSlicer.conf")
    t = open(p, encoding="utf-8").read()
    j = json.loads(t[:t.rfind("}") + 1])
    for k in ("access_code", "user_access_code", "local_machines"):
        j[k] = {}
    j["devices"] = []
    models = j.get("models") or []
    if not any(m.get("model") == model for m in models):
        models.append({"model": model, "nozzle_diameter": "0.4", "vendor": vendor})
    j["models"] = models
    j.setdefault("app", {})["keep_printer_on_open"] = "false"
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(j, indent=4) + "\n")
    hub = os.path.join(dd, "hub")
    st = {}
    sp = os.path.join(template, "hub", "settings.json")
    if os.path.isfile(sp):
        st = json.load(open(sp, encoding="utf-8"))
    st.update({"phone": True, "token": TOKEN, "remote_on": False, "notify": {"destinations": []}})
    if isinstance(st.get("apppush"), dict):
        st["apppush"]["enabled"] = False
    st.pop("spoolman", None)
    json.dump(st, open(os.path.join(hub, "settings.json"), "w", encoding="utf-8"), indent=1)
    json.dump(snapmaker_lan or {"devices": []}, open(os.path.join(hub, "snapmaker_lan.json"), "w"), indent=1)
    json.dump({"hosts": [], "active": []}, open(os.path.join(hub, "streams.json"), "w"))
    json.dump(print_host_devices or {"version": 1, "models": {}}, open(os.path.join(hub, "print_host_devices.json"), "w"), indent=1)
    os.makedirs(os.path.join(hub, "uploads", "gate"), exist_ok=True)
    shutil.copy2(fixture, os.path.join(hub, "uploads", "gate", os.path.basename(fixture)))
    return os.path.join(hub, "uploads", "gate", os.path.basename(fixture))


class Instance:
    """The install, started hidden on the gate's data dir (test_send_qidi_gate.Instance)."""

    def __init__(self, exe, dd, fixture):
        self.exe, self.dd, self.fixture = exe, dd, fixture
        self.proc = None
        self.pid = None
        self.api = None
        self.hub = None

    def start(self, timeout=600):
        env = dict(os.environ)
        env.update({"SNORCA_PHONE_ACCESS": TOKEN, "SNORCA_NEW_INSTANCE": "1", "SNORCA_HIDDEN": "1"})
        for k in ("SNORCA_SEND_DRYRUN", "SNORCA_SDCP_PORT"):
            env.pop(k, None)
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        self.proc = subprocess.Popen([self.exe, "--datadir", self.dd, self.fixture], env=env, creationflags=flags, close_fds=True)
        self.pid = self.proc.pid
        print("instance pid %d, data dir %s" % (self.pid, self.dd), flush=True)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                raise SystemExit("the instance exited (%s)" % self.proc.returncode)
            try:
                hub = json.load(open(os.path.join(self.dd, "hub", "hub.json"), encoding="utf-8"))
                api = "http://127.0.0.1:%d/r/%s/i/%d/api" % (hub["port"], TOKEN, self.pid)
                st, info = http(api + "/info", timeout=5)
                if st == 200 and info.get("title"):
                    self.api, self.hub = api, hub
                    print("ready: hub %d, '%s' (%.0f s)" % (hub["port"], info["title"], time.time() - t0), flush=True)
                    return
            except (OSError, ValueError, KeyError):
                pass
            time.sleep(2)
        raise SystemExit("the instance did not come up in %d s" % timeout)

    def call(self, path, method="GET", body=None, timeout=60):
        return http(self.api + path, method, body, timeout=timeout)

    def open_and_slice(self, ledger, opened, printer_preset):
        st, j = self.call("/project/open", "POST", urllib.parse.urlencode({"path": opened, "mode": "load"}), timeout=180)
        ledger.check(st == 200, "the fixture project opened through /api/project/open (%d %s)" % (st, j.get("error", "")))
        ps = {}
        t0 = time.time()
        while time.time() - t0 < 60:
            st, ps = self.call("/presets")
            if ps.get("printer_name") == printer_preset:
                break
            time.sleep(1)
        ledger.check(ps.get("printer_name") == printer_preset, "the project's printer preset is selected (%s)" % ps.get("printer_name"))
        st, j = self.call("/slice", "POST", "plate=0")
        ledger.check(st == 200 and "job" in j, "slice started (%d %s)" % (st, j.get("error", "")))
        if st == 200:
            r = self.wait_job(j["job"], 300)
            ledger.check(r.get("state") == "done", "plate sliced (%s %s)" % (r.get("state"), r.get("error", "")))

    def wait_job(self, job, timeout=150):
        t0 = time.time()
        while time.time() - t0 < timeout:
            st, j = self.call("/jobs/%d" % job)
            if st == 200 and j.get("state") not in ("running", "", None):
                j["_elapsed"] = time.time() - t0
                return j
            time.sleep(0.5)
        return {"state": "timeout", "error": "no end in %d s" % timeout}

    def send(self, printer, mode="print", **extra):
        form = dict({"printer": printer, "mode": mode, "confirm": "1"}, **extra)
        st, r = self.call("/plates/0/send", "POST", urllib.parse.urlencode(form), timeout=120)
        if st != 200 or "job" not in r:
            return {"state": "refused", "error": r.get("error", "HTTP %d" % st), "http": st}
        return self.wait_job(r["job"])

    def stop(self):
        if self.api:
            http(self.api + "/quit?discard=1", "POST", "via=gate", timeout=10)
        time.sleep(3)
        if self.hub:
            admin = self.hub.get("admin_port") or self.hub["port"]
            http("http://127.0.0.1:%d/hub/quit" % admin, "POST", "via=gate", timeout=10,
                 headers={"X-Hub-Secret": self.hub.get("secret", "")})
        if self.proc:
            try:
                self.proc.wait(30)
            except subprocess.TimeoutExpired:
                subprocess.run(["taskkill", "/PID", str(self.pid), "/T", "/F"], capture_output=True)
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='EdgeSlicer.exe'\" | Where-Object { $_.CommandLine -like '*%s*' }"
              " | ForEach-Object { $_.ProcessId }" % self.dd.replace("'", "''"))
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=60).stdout
            for pid in out.split():
                if pid.isdigit():
                    print("stopping leftover pid %s on the gate's data dir" % pid)
                    subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True)
        except (OSError, subprocess.SubprocessError):
            pass


def expect(ledger, emu, pid, what, kind, mark, **fields):
    from emu_assert import EmuAssertionError
    try:
        e = emu.expect(pid, kind=kind, since=mark, **fields)
        ledger.check(True, what)
        return e
    except EmuAssertionError as ex:
        ledger.check(False, "%s\n%s" % (what, ex))
        return None


def expect_none(ledger, emu, pid, what, kind, mark, **fields):
    from emu_assert import EmuAssertionError
    try:
        emu.expect_none(pid, kind=kind, since=mark, **fields)
        return ledger.check(True, what)
    except EmuAssertionError as ex:
        return ledger.check(False, "%s\n%s" % (what, ex))


def expect_order(ledger, emu, pid, what, kinds, mark):
    from emu_assert import EmuAssertionError
    try:
        emu.expect_order(pid, kinds, since=mark)
        return ledger.check(True, what)
    except EmuAssertionError as ex:
        return ledger.check(False, "%s\n%s" % (what, ex))

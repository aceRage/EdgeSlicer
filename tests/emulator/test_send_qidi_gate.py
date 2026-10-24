"""QIDI send gate (design section 5, Q1-Q13): EdgeSlicer's hub send path against the printer emulator.

An installed EdgeSlicer is started hidden on its own scratch data dir with ONE print-host device, a
QIDI X-Max 4 at a 127.0.0.x address served by the emulator. A two-filament PLA plate (made here
with the CLI) is sliced through the hub API and sent with POST /api/plates/0/send mode=print, once
per row, and each row asserts on the emulator's command log:

  Q1  Box, 4 slots, value_t0/1 stale            enable_box=1, value_t0=slot0, value_t1=slot1, read back,
                                                then upload, then start (mapping strictly before upload)
  Q2  Q1 + drop_save_variable  (#611 guard)     no upload, no start; "could not be confirmed"
  Q3  printing                                  no script, no upload, no start; "busy"
  Q4  Box read from save_variables + cfg only   slots from vars + officiall_filas_list.cfg; mapping sent
  Q5  Q4 + filament list 404 (Box unreadable)   policy refusal, nothing sent
  Q6  Q1 + refuse_g31                           the print still starts (the hub path sends no G31)
  Q7  no Box (box_count 0)                      no value_t writes; upload and start
  Q8  probe delayed 12 s (QIDI timeout is 10)   print refused, nothing sent; then mode=upload uploads
                                                only (no script, no start)
  Q9  PLA plate, only an ASA slot  (#611)       refused, nothing sent (never fed from the ASA slot)

The Box as the firmware lays it out (fix/qidi-box-index, the 2.4.6.3 report: all filament in Box 1,
1C ASA, shown as "Unit 2" PLA and T1 sent to slot7). save_variables keep filament_slot<n> = 1 (a PLA)
on every slot, empty or not; presence is the slot's state word slot<n> and its box_stepper sensor:

  Q10 save_variables only, 1A PLA red, 1B EMPTY    value_t0=slot0, value_t1=slot0 (the only PLA);
      (filament_slot1 still 1), 1C ASA, 1D PETG    never value_t -> slot1 (the empty slot)
  Q11 Q10's Box, flat multi_color_controller       the same mapping, read from box0_... keys
      (QIDIStudio's 2026-10-19 shape)
  Q12 the reported picture: save_variables say     refused as "did not add up", nothing sent
      box_count 2, the printer has one Box; Box 1
      spools untagged, Box 2 the PLA defaults
  Q13 two Boxes, filament only in Box 1            value_t0=slot0, value_t1=slot1; nothing to slot4-7

Every printer address is asserted to be loopback before anything starts. Nothing here writes to
%APPDATA%\\EdgeSlicer or the owner's emulator home; only processes this script started are stopped,
by PID.

usage: python tests/emulator/test_send_qidi_gate.py [--exe=C:/Dev/EdgeSlicerBuilds/current/EdgeSlicer.exe]
           [--work=<scratch dir>] [--template=<a data dir with the first-run wizard done>]
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from emu_proc import REPO, EmulatorProcess, assert_loopback, free_port  # noqa: E402
from emu_assert import Emulator, EmuAssertionError  # noqa: E402

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
EXE = opts.get("--exe", r"C:\Dev\EdgeSlicerBuilds\current\EdgeSlicer.exe")
WORK = os.path.abspath(opts.get("--work") or tempfile.mkdtemp(prefix="emu_qidi_gate_"))
TEMPLATE = opts.get("--template") or os.path.join(os.environ.get("LOCALAPPDATA", ""), "Temp", "snorca_hubtest", "dd_lan")
TOKEN = "testtoken12345"
PRINTER_ID = "gate-qidi"
DEVICE = "emuqidi01"
HOST = "127.0.0.%d" % (70 + os.getpid() % 100)
PORT = free_port(HOST)
ADDRESS = assert_loopback("%s:%d" % (HOST, PORT))
DD = os.path.join(WORK, "dd")
FIXTURE = os.path.join(WORK, "qidi_2pla.3mf")
MODEL = "Qidi X-Max 4"
RED, WHITE = "#E03030", "#F5F5F5"

fails, checks, rows = [], 0, {}
current_row = None


def check(cond, what):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append((current_row, what))
        rows[current_row] = False
    return cond


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


# ---- the fixture ---------------------------------------------------------------------------------
def make_fixture():
    """Two 20 mm cubes for the QIDI X-Max 4, both PLA: the first on filament 1 (red), the second on
    filament 2 (white). Exported as a project by the install's own CLI on a scratch data dir; the
    second object's filament is then set in Metadata/model_settings.config (the CLI's per-object
    --load-filament-ids needs filament files, and process-level wall/infill filaments do not
    survive the project load)."""
    import zipfile
    f = "QIDI PLA Rapido @Qidi X-Max 4 0.4 nozzle"
    cube = os.path.join(REPO, "tests", "data", "20mm_cube.obj")
    raw = os.path.join(WORK, "qidi_2pla_raw.3mf")
    cmd = [EXE, "--allow-newer-file", "--datadir", os.path.join(WORK, "clidd"),
           "--printer-preset", "Qidi X-Max 4 0.4 nozzle", "--process-preset", "0.20mm Standard @X-Max 4",
           "--filament-presets", "%s;%s" % (f, f), "--filament-colour", "%s;%s" % (RED, WHITE),
           "--arrange", "1", "--export-3mf", raw, cube, cube]
    with open(os.path.join(WORK, "cli_fixture.log"), "w", encoding="utf-8") as log:
        rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=300,
                            cwd=WORK).returncode
    if rc != 0 or not os.path.isfile(raw):
        raise SystemExit("could not make the fixture (CLI exit %s, see %s)" % (rc, os.path.join(WORK, "cli_fixture.log")))
    with zipfile.ZipFile(raw) as zin, zipfile.ZipFile(FIXTURE, "w", zipfile.ZIP_DEFLATED) as zout:
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


# ---- the data dir --------------------------------------------------------------------------------
def make_datadir():
    if not os.path.isdir(TEMPLATE) or not os.path.isfile(os.path.join(TEMPLATE, "EdgeSlicer.conf")):
        raise SystemExit("no template data dir at %s (one whose first-run wizard is done; --template=)" % TEMPLATE)
    os.makedirs(os.path.join(DD, "hub"), exist_ok=True)
    for name in ("EdgeSlicer.conf", "hms", "ota", "plugins", "printers", "system", "user", "web", "ultranet", "FlashNetwork"):
        src = os.path.join(TEMPLATE, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(DD, name))
        elif os.path.isfile(src):
            shutil.copy2(src, os.path.join(DD, name))
    p = os.path.join(DD, "EdgeSlicer.conf")
    t = open(p, encoding="utf-8").read()
    j = json.loads(t[:t.rfind("}") + 1])
    for k in ("access_code", "user_access_code", "local_machines"):
        j[k] = {}
    j["devices"] = []
    models = j.get("models") or []
    if not any(m.get("model") == MODEL for m in models):
        models.append({"model": MODEL, "nozzle_diameter": "0.4", "vendor": "Qidi"})
    j["models"] = models
    # The template keeps the owner's preferred printer when a project opens (keep_printer_on_open):
    # the gate needs the fixture's own QIDI printer and its two filaments.
    j.setdefault("app", {})["keep_printer_on_open"] = "false"
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(j, indent=4) + "\n")
    hub = os.path.join(DD, "hub")
    st = {}
    sp = os.path.join(TEMPLATE, "hub", "settings.json")
    if os.path.isfile(sp):
        st = json.load(open(sp, encoding="utf-8"))
    st.update({"phone": True, "token": TOKEN, "remote_on": False, "notify": {"destinations": []}})
    if isinstance(st.get("apppush"), dict):
        st["apppush"]["enabled"] = False
    st.pop("spoolman", None)
    json.dump(st, open(os.path.join(hub, "settings.json"), "w", encoding="utf-8"), indent=1)
    json.dump({"devices": []}, open(os.path.join(hub, "snapmaker_lan.json"), "w"))
    json.dump({"hosts": [], "active": []}, open(os.path.join(hub, "streams.json"), "w"))
    devices = {"version": 1, "models": {MODEL: {"current": DEVICE, "migrated": [], "devices": [{
        "id": DEVICE, "alias": "EMU QIDI X-Max 4", "address": ADDRESS, "host_type": "octoprint", "auth_type": "key",
        "apikey": "", "user": "", "password": "", "printer_model": MODEL, "created": 1760000000, "last_used": 0}]}}}
    json.dump(devices, open(os.path.join(hub, "print_host_devices.json"), "w"), indent=1)
    # /api/project/open only opens files under <hub>/uploads/<dir>/, where a phone upload lands.
    os.makedirs(os.path.join(hub, "uploads", "gate"), exist_ok=True)
    shutil.copy2(FIXTURE, os.path.join(hub, "uploads", "gate", os.path.basename(FIXTURE)))
    # Belt and braces: every address any print-host device in this data dir names is loopback.
    for m in devices["models"].values():
        for d in m["devices"]:
            assert_loopback(d["address"])


# ---- the instance --------------------------------------------------------------------------------
class Instance:
    def __init__(self):
        self.proc = None
        self.pid = None
        self.api = None
        self.hub = None

    def start(self):
        env = dict(os.environ)
        env.update({"SNORCA_PHONE_ACCESS": TOKEN, "SNORCA_NEW_INSTANCE": "1", "SNORCA_HIDDEN": "1"})
        env.pop("SNORCA_SEND_DRYRUN", None)
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        # The fixture on the command line gets the instance up (a hidden start with no file stalled
        # here). The gate then opens it again through the hub's /api/project/open, as the phone does.
        self.proc = subprocess.Popen([EXE, "--datadir", DD, FIXTURE], env=env, creationflags=flags, close_fds=True)
        self.pid = self.proc.pid
        print("instance pid %d, data dir %s" % (self.pid, DD), flush=True)
        t0 = time.time()
        while time.time() - t0 < 600:
            if self.proc.poll() is not None:
                raise SystemExit("the instance exited (%s)" % self.proc.returncode)
            try:
                hub = json.load(open(os.path.join(DD, "hub", "hub.json"), encoding="utf-8"))
                api = "http://127.0.0.1:%d/r/%s/i/%d/api" % (hub["port"], TOKEN, self.pid)
                st, info = http(api + "/info", timeout=5)
                if st == 200 and info.get("title"):
                    self.api, self.hub = api, hub
                    print("ready: hub %d, '%s' (%.0f s)" % (hub["port"], info["title"], time.time() - t0), flush=True)
                    return
            except (OSError, ValueError, KeyError):
                pass
            time.sleep(2)
        raise SystemExit("the instance did not come up in 600 s")

    def call(self, path, method="GET", body=None, timeout=60):
        return http(self.api + path, method, body, timeout=timeout)

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
        # Anything still running on THIS scratch data dir was started by this gate (a hub keeper).
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='EdgeSlicer.exe'\" | Where-Object { $_.CommandLine -like '*%s*' }"
              " | ForEach-Object { $_.ProcessId }" % DD.replace("'", "''"))
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=60).stdout
            for pid in out.split():
                if pid.isdigit():
                    print("stopping leftover pid %s on the gate's data dir" % pid)
                    subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True)
        except (OSError, subprocess.SubprocessError):
            pass


def wait_job(inst, job, timeout=150):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, j = inst.call("/jobs/%d" % job)
        if st == 200 and j.get("state") not in ("running", "", None):
            j["_elapsed"] = time.time() - t0
            return j
        time.sleep(0.5)
    return {"state": "timeout", "error": "no end in %d s" % timeout}


def send(inst, mode="print"):
    form = urllib.parse.urlencode({"printer": "ph:" + DEVICE, "mode": mode, "confirm": "1"})
    st, r = inst.call("/plates/0/send", "POST", form, timeout=120)
    if st != 200 or "job" not in r:
        return {"state": "refused", "error": r.get("error", "HTTP %d" % st), "http": st}
    return wait_job(inst, r["job"])


# ---- the matrix ----------------------------------------------------------------------------------
SLOTS_Q1 = [{"type": "PLA", "sub": "PLA", "vendor": "Generic", "color": RED, "loaded": True},
            {"type": "PLA", "sub": "PLA Matte", "vendor": "Generic", "color": WHITE, "loaded": True},
            {"type": "ASA", "sub": "ASA", "vendor": "Generic", "color": "#202020", "loaded": True},
            {"type": "PETG", "sub": "PETG", "vendor": "Generic", "color": "#FFFFFF", "loaded": True}]
BASE = {"state": "standby", "qidi": {"box_report": "mcc+vars", "boxes": 1, "polar_cooler": True,
                                     "stale_value_t": {"0": "slot3", "1": "slot2"}},
        "slots": SLOTS_Q1, "faults": {"drop_save_variable": False, "refuse_g31": False, "filas_cfg_404": False,
                                      "probe_delay_s": 0, "box_unplugged": False, "box_unreadable": False}}


def setup(emu, **changes):
    patch = json.loads(json.dumps(BASE))
    for k, v in changes.items():
        if isinstance(v, dict) and isinstance(patch.get(k), dict):
            patch[k].update(v)
        else:
            patch[k] = v
    return emu.reset(PRINTER_ID, patch)


def row(name, title):
    global current_row
    current_row = name
    rows[name] = True
    print("\n-- %s: %s --" % (name, title), flush=True)


def nothing_sent(emu, mark, label):
    for kind in ("save_variable", "script", "upload", "start"):
        try:
            emu.expect_none(PRINTER_ID, kind=kind, since=mark)
            check(True, "%s: no %s" % (label, kind))
        except EmuAssertionError as e:
            check(False, "%s: no %s\n%s" % (label, kind, e))


def expect(emu, what, kind, mark, **fields):
    try:
        e = emu.expect(PRINTER_ID, kind=kind, since=mark, **fields)
        check(True, what)
        return e
    except EmuAssertionError as ex:
        check(False, "%s\n%s" % (what, ex))
        return None


def expect_order(emu, what, kinds, mark):
    try:
        emu.expect_order(PRINTER_ID, kinds, since=mark)
        return check(True, what)
    except EmuAssertionError as ex:
        return check(False, "%s\n%s" % (what, ex))


def run_matrix(inst, emu):
    # Q1 ---------------------------------------------------------------------------------------
    row("Q1", "Box, 4 slots, stale value_t0=slot3 / value_t1=slot2; red + white PLA plate")
    mark = setup(emu)
    j = send(inst)
    check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    q = (j.get("result") or {}).get("qidi") or {}
    check(q.get("mapping") == "0:0,1:1" and q.get("sent") is True, "hub result: mapping 0:0,1:1 sent and confirmed (%s)" % q)
    expect(emu, "SAVE_VARIABLE enable_box=1", "save_variable", mark, var="enable_box", value=1, stored=True)
    expect(emu, "SAVE_VARIABLE value_t0='slot0' (was slot3)", "save_variable", mark, var="value_t0", value="slot0")
    expect(emu, "SAVE_VARIABLE value_t1='slot1' (was slot2)", "save_variable", mark, var="value_t1", value="slot1")
    expect_order(emu, "order: probe, mapping, read-back, upload, start", ["probe", "save_variable", "readback", "upload", "start"], mark)
    up = expect(emu, "upload recorded with name, size, sha256", "upload", mark)
    st = emu.state(PRINTER_ID)
    check(st["save_variables"].get("value_t0") == "slot0" and st["save_variables"].get("value_t1") == "slot1",
          "the printer's variables now say slot0 / slot1")
    check(st["print_state"] == "printing", "the printer is printing")
    src = (j.get("result") or {}).get("source") or ""
    if up and os.path.isfile(src):
        h = hashlib.sha256(open(src, "rb").read()).hexdigest()
        check(up["payload"]["sha256"] == h and up["payload"]["size"] == os.path.getsize(src),
              "the uploaded bytes are the plate's G-code (sha256 %s..., %d bytes)" % (h[:12], up["payload"]["size"]))
    start = expect(emu, "start of the uploaded file", "start", mark)
    if up and start:
        check(start["payload"].get("file") == up["payload"]["name"], "the start names the uploaded file")

    # Q2 ---------------------------------------------------------------------------------------
    row("Q2", "Q1 + drop_save_variable (the printer answers ok, stores nothing) - the #611 guard")
    mark = setup(emu, faults={"drop_save_variable": True})
    j = send(inst)
    check(j.get("state") == "error", "the send job fails (%s)" % j.get("state"))
    check("could not be confirmed" in (j.get("error") or ""), "error says the mapping could not be confirmed: %s" % j.get("error"))
    expect(emu, "the mapping went out (and was dropped)", "save_variable", mark, var="value_t0", stored=False)
    expect(emu, "it was read back", "readback", mark)
    for kind in ("upload", "start"):
        try:
            emu.expect_none(PRINTER_ID, kind=kind, since=mark)
            check(True, "no %s" % kind)
        except EmuAssertionError as e:
            check(False, "no %s\n%s" % (kind, e))
    check(emu.state(PRINTER_ID)["print_state"] == "standby", "the printer is still idle")

    # Q3 ---------------------------------------------------------------------------------------
    row("Q3", "the printer is printing")
    mark = setup(emu, state="printing")
    j = send(inst)
    check(j.get("state") == "error" and "busy" in (j.get("error") or ""), "refused as busy: %s" % j.get("error"))
    nothing_sent(emu, mark, "busy")

    # Q4 ---------------------------------------------------------------------------------------
    row("Q4", "Box read from save_variables + officiall_filas_list.cfg only (multi_color_controller names nothing)")
    mark = setup(emu, qidi={"box_report": "vars_only"})
    j = send(inst)
    check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    expect(emu, "the filament list was read", "filas_cfg", mark)
    expect(emu, "value_t0='slot0' from the vars", "save_variable", mark, var="value_t0", value="slot0")
    expect(emu, "value_t1='slot1' from the vars", "save_variable", mark, var="value_t1", value="slot1")
    expect_order(emu, "order: filament list, mapping, read-back, upload, start",
                 ["filas_cfg", "save_variable", "readback", "upload", "start"], mark)

    # Q5 ---------------------------------------------------------------------------------------
    row("Q5", "Q4 + officiall_filas_list.cfg 404: a Box nobody can read")
    mark = setup(emu, qidi={"box_report": "vars_only"}, faults={"filas_cfg_404": True})
    j = send(inst)
    check(j.get("state") == "error" and "no QIDI Box slot" in (j.get("error") or ""),
          "policy refusal (needs mapping): %s" % j.get("error"))
    nothing_sent(emu, mark, "unreadable Box")

    # Q6 ---------------------------------------------------------------------------------------
    row("Q6", "Q1 + refuse_g31")
    mark = setup(emu, faults={"refuse_g31": True})
    j = send(inst)
    check(j.get("state") == "done", "the print still goes ahead (%s %s)" % (j.get("state"), j.get("error", "")))
    expect(emu, "started", "start", mark)
    g31 = emu.find(PRINTER_ID, kind="gcode", since=mark, cmd="G31")
    if g31:
        check(all(e["payload"].get("refused") for e in g31), "G31 was sent and refused, print went ahead")
    else:
        print("  note G31 never sent: the hub send path has no bed-leveling option (desktop-only, hand-test card)")

    # Q7 ---------------------------------------------------------------------------------------
    row("Q7", "no Box (box_count 0)")
    mark = setup(emu, qidi={"boxes": 0}, slots=[])
    j = send(inst)
    check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    vt = [e for e in emu.find(PRINTER_ID, kind="save_variable", since=mark) if str(e["payload"].get("var", "")).startswith("value_t")]
    check(not vt, "no value_t writes (%d)" % len(vt))
    check(not emu.find(PRINTER_ID, kind="save_variable", since=mark, var="enable_box", value=1), "enable_box=1 never sent")
    eb0 = emu.find(PRINTER_ID, kind="save_variable", since=mark, var="enable_box", value=0)
    print("  note enable_box=0 %s" % ("sent" if eb0 else "not sent: the hub sends no Box script at all without a Box"))
    expect_order(emu, "upload, then start", ["upload", "start"], mark)

    # Q8 ---------------------------------------------------------------------------------------
    row("Q8", "probe delayed 12 s (over the 10 s QIDI probe timeout)")
    mark = setup(emu, faults={"probe_delay_s": 12})
    j = send(inst)
    check(j.get("state") == "error" and "Couldn't read the QIDI Box" in (j.get("error") or ""),
          "print refused after the probe timed out (%.0f s): %s" % (j.get("_elapsed", 0), j.get("error")))
    nothing_sent(emu, mark, "probe timeout")
    mark = emu.mark()
    j = send(inst, mode="upload")
    check(j.get("state") == "done", "mode=upload still works (%s %s)" % (j.get("state"), j.get("error", "")))
    expect(emu, "upload only: print=false", "upload", mark, print=False)
    for kind in ("save_variable", "start"):
        try:
            emu.expect_none(PRINTER_ID, kind=kind, since=mark)
            check(True, "upload only: no %s" % kind)
        except EmuAssertionError as e:
            check(False, "upload only: no %s\n%s" % (kind, e))

    # Q9 ---------------------------------------------------------------------------------------
    row("Q9", "PLA plate, the Box holds only ASA (#611: never feed PLA settings from the ASA slot)")
    mark = setup(emu, slots=[{"type": "ASA", "sub": "ASA", "color": RED, "loaded": True}, {"loaded": False},
                             {"loaded": False}, {"loaded": False}], qidi={"stale_value_t": {"0": "slot0", "1": "slot0"}})
    j = send(inst)
    check(j.get("state") == "error" and "no QIDI Box slot of the same material" in (j.get("error") or ""),
          "refused, needs a PLA slot: %s" % j.get("error"))
    nothing_sent(emu, mark, "ASA slot only")
    check(emu.state(PRINTER_ID)["print_state"] == "standby", "the printer is still idle")

    # Q10-Q13: the Box as the firmware lays it out -----------------------------------------------
    box1 = [{"type": "PLA", "sub": "PLA", "vendor": "Generic", "color": RED, "loaded": True},
            {"loaded": False},  # filament_slot1 keeps the shipped 1 (a white PLA)
            {"type": "ASA", "sub": "ASA", "vendor": "Generic", "color": "#202020", "loaded": True},
            {"type": "PETG", "sub": "PETG", "vendor": "Generic", "color": "#FFFFFF", "loaded": True}]

    def never_to(mark, slots, label):
        bad = [e for e in emu.find(PRINTER_ID, kind="save_variable", since=mark)
               if str(e["payload"].get("var", "")).startswith("value_t") and e["payload"].get("value") in slots]
        check(not bad, "%s (%s)" % (label, [(e["payload"]["var"], e["payload"]["value"]) for e in bad]))

    row("Q10", "save_variables only, the firmware's layout: 1B empty but filament_slot1 still the shipped PLA")
    mark = setup(emu, qidi={"box_report": "vars_only", "mcc_shape": "array", "stale_value_t": {"0": "slot1", "1": "slot1"}},
                 slots=box1)
    j = send(inst)
    q = (j.get("result") or {}).get("qidi") or {}
    check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    check(q.get("mapping") == "0:0,1:0", "both PLA filaments go to 1A, the only loaded PLA (%s)" % q.get("mapping"))
    expect(emu, "value_t0='slot0'", "save_variable", mark, var="value_t0", value="slot0")
    expect(emu, "value_t1='slot0'", "save_variable", mark, var="value_t1", value="slot0")
    never_to(mark, ["slot1"], "nothing pointed at the empty 1B (slot1)")

    row("Q11", "Q10's Box through a flat multi_color_controller (box0_... keys) and save_variables")
    mark = setup(emu, qidi={"box_report": "mcc+vars", "mcc_shape": "flat", "stale_value_t": {"0": "slot1", "1": "slot1"}},
                 slots=box1)
    j = send(inst)
    q = (j.get("result") or {}).get("qidi") or {}
    check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    check(q.get("mapping") == "0:0,1:0", "mapping 0:0,1:0 from the flat keys (%s)" % q.get("mapping"))
    never_to(mark, ["slot1"], "nothing pointed at the empty 1B (slot1)")
    check("filas_cfg" not in [e["kind"] for e in emu.find(PRINTER_ID, since=mark)],
          "the controller named the materials: no filament list needed")

    row("Q12", "the 2.4.6.3 picture: save_variables say 2 Boxes, the printer has 1; Box 1 untagged, 1C ASA")
    untagged = [{"type": "PLA", "color": RED, "loaded": True, "untagged": True},
                {"type": "PLA", "color": WHITE, "loaded": True, "untagged": True},
                {"type": "ASA", "color": "#202020", "loaded": True, "untagged": True},
                {"loaded": False}]
    mark = setup(emu, qidi={"box_report": "vars_only", "mcc_shape": "array", "boxes": 1, "vars_box_count": 2,
                            "stale_value_t": {"0": "slot7", "1": "slot7"}}, slots=untagged)
    j = send(inst)
    q = (j.get("result") or {}).get("qidi") or {}
    check(j.get("state") == "error" and "did not add up" in (j.get("error") or ""),
          "refused, the Box answer does not add up: %s" % j.get("error"))
    check(bool(q.get("box_problem")), "the hub result says why (%s)" % q.get("box_problem"))
    nothing_sent(emu, mark, "inconsistent Box")
    check(emu.state(PRINTER_ID)["print_state"] == "standby", "the printer is still idle")
    mark = emu.mark()
    j = send(inst, mode="upload")
    check(j.get("state") == "done", "mode=upload still works (%s %s)" % (j.get("state"), j.get("error", "")))
    expect(emu, "upload only: print=false", "upload", mark, print=False)

    row("Q13", "two Boxes, filament only in Box 1: Box 2's PLA defaults are not filament")
    two = [{"type": "PLA", "sub": "PLA", "vendor": "Generic", "color": RED, "loaded": True},
           {"type": "PLA", "sub": "PLA", "vendor": "Generic", "color": WHITE, "loaded": True},
           {"type": "ASA", "sub": "ASA", "vendor": "Generic", "color": "#202020", "loaded": True},
           {"loaded": False}, {"loaded": False}, {"loaded": False}, {"loaded": False}, {"loaded": False}]
    mark = setup(emu, qidi={"box_report": "vars_only", "mcc_shape": "array", "boxes": 2,
                            "stale_value_t": {"0": "slot4", "1": "slot5"}}, slots=two)
    j = send(inst)
    q = (j.get("result") or {}).get("qidi") or {}
    check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    check(q.get("mapping") == "0:0,1:1", "red to 1A, white to 1B (%s)" % q.get("mapping"))
    never_to(mark, ["slot%d" % n for n in range(3, 8)], "nothing pointed at an empty slot (1D, 2A-2D)")


def main():
    global current_row
    os.makedirs(WORK, exist_ok=True)
    if not os.path.isfile(EXE):
        raise SystemExit("no EdgeSlicer.exe at %s" % EXE)
    print("gate work dir %s\nexe %s\nemulated QIDI at %s (loopback)" % (WORK, EXE, ADDRESS), flush=True)
    make_fixture()
    make_datadir()
    printer = {"id": PRINTER_ID, "personality": "qidi", "name": "EMU gate QIDI X-Max 4", "bind": HOST, "port": PORT}
    printer.update(json.loads(json.dumps(BASE)))
    emu_p = EmulatorProcess(os.path.join(WORK, "emuhome"), [printer])
    inst = Instance()
    try:
        emu_p.start()
        emu = Emulator(emu_p.control)
        inst.start()
        current_row = "setup"
        rows["setup"] = True
        opened = os.path.join(DD, "hub", "uploads", "gate", os.path.basename(FIXTURE))
        st, j = inst.call("/project/open", "POST", urllib.parse.urlencode({"path": opened, "mode": "load"}), timeout=180)
        check(st == 200, "the fixture project opened through /api/project/open (%d %s)" % (st, j.get("error", "")))
        t0 = time.time()
        while time.time() - t0 < 60:
            st, ps = inst.call("/presets")
            if ps.get("printer_name") == "Qidi X-Max 4 0.4 nozzle":
                break
            time.sleep(1)
        check(ps.get("printer_name") == "Qidi X-Max 4 0.4 nozzle", "the project's printer preset is selected (%s)" % ps.get("printer_name"))
        st, j = inst.call("/slice", "POST", "plate=0")
        check(st == 200 and "job" in j, "slice started (%d %s)" % (st, j.get("error", "")))
        if st == 200:
            r = wait_job(inst, j["job"], 300)
            check(r.get("state") == "done", "plate sliced (%s %s)" % (r.get("state"), r.get("error", "")))
        st, pl = inst.call("/printers?plate=0")
        dev = next((p for p in pl.get("printers", []) if p.get("id") == "ph:" + DEVICE), {})
        check(dev.get("can_print") is True and dev.get("url") == ADDRESS,
              "ph:%s listed with can_print at %s (%s)" % (DEVICE, ADDRESS, {k: dev.get(k) for k in ("can_print", "url", "model")}))
        assert_loopback(dev.get("url") or ADDRESS)
        if not fails:
            run_matrix(inst, emu)
    finally:
        inst.stop()
        emu_p.stop()
        print("(work dir with the instance, emulator and CLI logs: %s)" % WORK)
    print("\n== QIDI send gate: %d checks, %d failed ==" % (checks, len(fails)))
    for name in ["setup"] + ["Q%d" % i for i in range(1, 14)]:
        if name in rows:
            print("  %-6s %s" % (name, "PASS" if rows[name] else "FAIL"))
    for r, f in fails:
        print("  FAILED [%s]: %s" % (r, f))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

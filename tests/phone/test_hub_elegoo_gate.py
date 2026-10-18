"""Gate for the hub's Elegoo SDCP backend, against a running test instance and mock_elegoo_sdcp.py.

The instance must have been started with SNORCA_SDCP_PORT=<the mock's port> and a data dir whose
hub/print_host_devices.json lists one Elegoo Centauri Carbon device at 127.0.0.1 (id elegoomock01),
under its own printer model while another model's preset is selected (gate_hubelegoo.sh sets this
up). Covers:
  * GET /api/printers: the ph: row carries state, progress, time left, layers, the file, bed / nozzle /
    chamber, fans, light, the CANVAS trays as `ams` (named, tray in use marked), the camera hint, and
    is listed although the selected preset is another model (no send: model_matches false);
  * it keeps working mid-print: the pushed progress moves on;
  * /summary carries the same through to the app;
  * pause / resume / stop, the light and a fan go to the printer over the same connection; a
    temperature is refused (read-only); stop needs confirm=1;
  * exactly one websocket to the printer for all of it;
  * printer refusing connections / gone: a clean offline row (no temperatures, no job), and back.
Nothing is ever sent to a real printer: the only printer is the mock on 127.0.0.1.

usage: test_hub_elegoo_gate.py --datadir=<the instance's data dir> --mock=127.0.0.1:<port> [--token=testtoken12345]
"""
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
DATADIR = opts.get("--datadir")
MOCK = opts.get("--mock", "127.0.0.1:18393")
TOKEN = opts.get("--token", "testtoken12345")
PID_ID = "ph:elegoomock01"
if not DATADIR:
    raise SystemExit(__doc__)
fails = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)


def http(url, method="GET", body=None, timeout=60, headers=None):
    req = urllib.request.Request(url, method=method, data=body.encode() if isinstance(body, str) else body,
                                 headers=headers or {})
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
    except Exception:
        return st, {"raw": raw[:400].decode("utf-8", "replace")}


hubj = json.load(open(os.path.join(DATADIR, "hub", "hub.json"), encoding="utf-8"))
HUB = "http://127.0.0.1:%d" % hubj["port"]
ADMIN = "http://127.0.0.1:%d" % (hubj.get("admin_port") or hubj["port"])
st, inst = http(ADMIN + "/hub/instances", headers={"X-Hub-Secret": hubj.get("secret", "")})
live = [i for i in (inst.get("instances") or []) if i.get("title")]
check(bool(live), "hub %s lists a live instance" % HUB)
if not live:
    sys.exit(1)
PID = live[0]["pid"]
API = "%s/r/%s/i/%d/api" % (HUB, TOKEN, PID)
MOCK_URL = "http://" + MOCK


def api(path, method="GET", body=None, timeout=60):
    return http(API + path, method, body, timeout=timeout)


def mock(path):
    return http(MOCK_URL + path, timeout=10)[1]


def row():
    st, pl = api("/printers")
    for p in pl.get("printers", []):
        if p.get("id") == PID_ID:
            return p
    return {}


def until(pred, timeout=20):
    p = {}
    t0 = time.time()
    while time.time() - t0 < timeout:
        p = row()
        if pred(p):
            return p
        time.sleep(1)
    return p


def control(**form):
    return api("/printers/%s/control" % urllib.parse.quote(PID_ID, safe=""), "POST", urllib.parse.urlencode(form))


def wait_job(job, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, j = api("/jobs/%d" % job)
        if st == 200 and j.get("state") != "running":
            return j
        time.sleep(0.5)
    return {"state": "timeout"}


def heater(p, hid):
    for h in (p.get("controls") or {}).get("heaters", []):
        if h.get("id") == hid:
            return h
    return {}


def fan(p, fid):
    for f in (p.get("controls") or {}).get("fans", []):
        if f.get("id") == fid:
            return f
    return {}


print("-- the mock as a Centauri Carbon 1, mid-print with a CANVAS --", flush=True)
check(bool(mock("/mock/reset")), "the mock at %s answers" % MOCK)
mock("/mock/print?state=printing")
mock("/mock/canvas?attached=1")

p = until(lambda p: p.get("status") == "printing", 30)
check(p.get("status") == "printing", "the CC1 row says printing (%s)" % p.get("status"))
check(p.get("print_status") == "printing", "print_status printing")
check(p.get("online") is True, "online")
check(p.get("kind") == "printhost" and p.get("model") == "Elegoo Centauri Carbon", "kind printhost, model Elegoo Centauri Carbon")
check(p.get("model_matches") is False and p.get("can_upload") is False,
      "listed while another model's preset is selected, without a send")
check(p.get("printing") is True, "printing flag")
check(isinstance(p.get("percent"), int) and p["percent"] >= 17, "percent %s" % p.get("percent"))
check(isinstance(p.get("left_time_s"), int) and 0 < p["left_time_s"] <= 3600, "left_time_s %s" % p.get("left_time_s"))
check(p.get("total_layers") == 150 and isinstance(p.get("layer"), int), "layer %s / %s" % (p.get("layer"), p.get("total_layers")))
check(p.get("task") == "Cube_PLA_mock.gcode", "the file name")
check(p.get("can_pause") is True and p.get("can_stop") is True and p.get("can_resume") is False, "pause / stop offered")
check(abs(p.get("bed_temp", 0) - 60.0) < 0.01 and p.get("bed_target") == 60, "bed 60 / 60")
check(p.get("nozzles") and abs(p["nozzles"][0]["temp"] - 220.0) < 0.01, "nozzle 220")
check(heater(p, "chamber").get("label") == "Chamber" and heater(p, "chamber").get("settable") is False, "chamber heater, read-only")
check(heater(p, "bed").get("settable") is False, "bed read-only")
check([f.get("id") for f in (p.get("controls") or {}).get("fans", [])] == ["part", "aux", "chamber"], "three fans")
check(fan(p, "part").get("percent") == 100, "part fan 100%")
check(((p.get("controls") or {}).get("light") or {}).get("on") is True, "light on")
check((p.get("camera_hint") or {}).get("available") is True and (p.get("camera_hint") or {}).get("rkind") == "elegoo",
      "camera hint: a camera is there, played as the Stream tab's elegoo tile")
check((p.get("sdcp") or {}).get("firmware") == "V1.4.49", "firmware from Cmd 1")

# GET /api/printers through the hub counts as somebody watching: the CANVAS is read.
p = until(lambda p: bool(p.get("ams")), 25)
ams = p.get("ams") or []
check(len(ams) == 1, "one CANVAS unit")
if ams:
    u = ams[0]
    check(u.get("name") == "CANVAS 1" and u.get("type") == "canvas", "named CANVAS 1, type canvas")
    trays = u.get("trays") or []
    check(len(trays) == 4, "four trays")
    if len(trays) == 4:
        check(trays[0].get("exists") is True and trays[0].get("color") == "#1E7A3C" and trays[0].get("type") == "PLA",
              "tray 1: green PLA")
        check(trays[1].get("exists") is False, "tray 2 empty")
        check(trays[3].get("active") is True and trays[3].get("sub_type") == "Generic PLA PRO",
              "tray 4: the untagged spool is Generic PLA PRO, in the toolhead")
        check("can_load" not in trays[0], "no load / unload offered")

print("-- mid-print --", flush=True)
before = p.get("percent", 0), p.get("left_time_s", 0)
time.sleep(12)
p = row()
check(p.get("status") == "printing" and p.get("left_time_s", 0) < before[1], "the pushed status moves on (%s -> %s s left)"
      % (before[1], p.get("left_time_s")))

print("-- /summary --", flush=True)
t0, srow = time.time(), {}
while time.time() - t0 < 30:
    st, s = http("%s/r/%s/summary" % (HUB, TOKEN))
    srow = next((r for r in s.get("printers", []) if r.get("id") == PID_ID), {})
    if srow.get("ams") and srow.get("state") == "printing":
        break
    time.sleep(2)
check(srow.get("state") == "printing", "/summary state printing")
check(bool(srow.get("ams")) and srow["ams"][0].get("name") == "CANVAS 1", "/summary carries the CANVAS")
check(bool(srow.get("controls")), "/summary carries controls")
check(isinstance(srow.get("chamber_temp"), (int, float)), "/summary carries the chamber reading")
check(bool(srow.get("camera_hint")) and bool(srow.get("sdcp")), "/summary carries camera_hint and sdcp")
# The camera: the Stream tab's CC1 tile (#599, rkind elegoo) at the printer's address is joined to the
# row, /state lists it as rkind elegoo, and the hub's relay route answers for it (nothing listens on
# 127.0.0.1's camera port, so it can only say why there is no picture - never a real printer).
check(srow.get("camera") == "cc1cam", "/summary joins the Stream tab's CC1 tile to the row (%s)" % srow.get("camera"))
st, state = http("%s/r/%s/state" % (HUB, TOKEN))
tile = next((h for h in state.get("hosts", []) if h.get("id") == "cc1cam"), {})
check(tile.get("rkind") == "elegoo", "/state lists the tile as rkind elegoo (%s)" % tile.get("rkind"))
st, cam = http("%s/r/%s/elegoo/status?id=cc1cam" % (HUB, TOKEN), timeout=20)
check(st == 200 and "state" in cam, "the hub's /elegoo/status route answers for the tile (%d %s)" % (st, cam.get("state")))
check(srow.get("percent", 0) > 0 and srow.get("job") == "Cube_PLA_mock.gcode", "/summary percent and job")

print("-- controls --", flush=True)
st, j = control(action="set_temp", heater="bed", target="50")
check(st == 409, "set_temp is refused (read-only): %d %s" % (st, j.get("error")))
st, j = control(action="stop")
check(st == 400, "stop without confirm=1 is refused (%d)" % st)
st, j = control(action="pause")
check(st == 200 and j.get("job"), "pause accepted (%d %s)" % (st, j.get("error", "")))
if st == 200:
    r = wait_job(j["job"])
    check(r.get("state") == "done", "pause job done (%s %s)" % (r.get("state"), r.get("error", "")))
p = until(lambda p: p.get("status") == "paused", 15)
check(p.get("status") == "paused" and p.get("can_resume") is True, "row paused, resume offered")
check(mock("/mock/state")["state"]["print"] == 6, "the mock printer is paused")
st, j = control(action="pause")
check(st == 409, "pausing a paused printer is refused (%d)" % st)
st, j = control(action="resume")
check(st == 200, "resume accepted (%d %s)" % (st, j.get("error", "")))
if st == 200:
    wait_job(j["job"])
p = until(lambda p: p.get("status") == "printing" and p.get("can_pause") is True, 15)
check(p.get("can_pause") is True, "printing again")
st, j = control(action="set_light", on="0")
check(st == 200, "set_light accepted (%d %s)" % (st, j.get("error", "")))
if st == 200:
    wait_job(j["job"])
p = until(lambda p: ((p.get("controls") or {}).get("light") or {}).get("on") is False, 10)
check(mock("/mock/state")["state"]["light"] == 0, "the mock's light is off")
check(((p.get("controls") or {}).get("light") or {}).get("on") is False, "the row's light is off")
st, j = control(action="set_fan", fan="chamber", percent="60")
check(st == 200, "set_fan accepted (%d %s)" % (st, j.get("error", "")))
if st == 200:
    wait_job(j["job"])
p = until(lambda p: fan(p, "chamber").get("percent") == 60, 10)
check(mock("/mock/state")["state"]["box_fan"] == 60, "the mock's chamber fan is 60%")
st, j = control(action="set_fan", fan="cavity", percent="60")
check(st == 404, "an unknown fan is 404 (%d)" % st)
st, j = control(action="stop", confirm="1")
check(st == 200, "stop accepted (%d %s)" % (st, j.get("error", "")))
if st == 200:
    wait_job(j["job"])
p = until(lambda p: p.get("status") == "cancelled", 15)
check(p.get("status") == "cancelled" and p.get("printing") is False, "the row says cancelled")
cmds = [c[0] for c in mock("/mock/state")["commands"]]
check(129 in cmds and 131 in cmds and 130 in cmds and 403 in cmds and 324 in cmds,
      "the printer saw 129, 131, 130, 403 and 324 (%s)" % sorted(set(cmds)))

st_m = mock("/mock/state")
check(st_m.get("max_open") == 1,
      "never more than one websocket (max open %s; %s connection(s) in all while somebody looked)"
      % (st_m.get("max_open"), st_m.get("connections_total")))

print("-- printer off --", flush=True)
mock("/mock/refuse?on=1")
mock("/mock/drop")
p = until(lambda p: p.get("online") is False, 20)
check(p.get("online") is False and p.get("status") == "offline", "offline row (%s)" % p.get("status"))
for k in ("bed_temp", "nozzles", "controls", "ams", "percent", "task"):
    check(k not in p, "offline row has no %s" % k)
check(bool(p.get("status_error")), "offline row says why: %s" % p.get("status_error"))
st, j = control(action="pause")
check(st == 409, "pause on an offline printer is refused (%d)" % st)
t0 = time.time()
api("/printers")
check(time.time() - t0 < 5, "listing printers stays fast with the printer off (%.1f s)" % (time.time() - t0))
mock("/mock/refuse?on=0")
mock("/mock/print?state=idle")
p = until(lambda p: p.get("online") is True and p.get("status") == "standby", 40)
check(p.get("status") == "standby", "back online, idle (%s)" % p.get("status"))

print("-- nobody looking: idle close, check-ins, a print held for its notifications --", flush=True)
# From here on only the mock is asked; the hub's background poll says watching=0 once 45 s pass.
t0 = time.time()
while time.time() - t0 < 120 and mock("/mock/state").get("open") != 0:
    time.sleep(2)
check(mock("/mock/state").get("open") == 0,
      "idle: no socket to the printer once nobody looks (%.0f s after the last look)" % (time.time() - t0))
time.sleep(3)
check(mock("/mock/state").get("open") == 0, "idle steady state: 0 sockets")
total = mock("/mock/state").get("connections_total", 0)
t0, seen_open = time.time(), False
while time.time() - t0 < 60:
    s = mock("/mock/state")
    seen_open = seen_open or s.get("open") == 1
    if s.get("connections_total", 0) > total and s.get("open") == 0:
        break
    time.sleep(0.5)
s = mock("/mock/state")
check(s.get("connections_total", 0) > total and s.get("open") == 0,
      "a check-in: connected, read the status and disconnected (%.0f s; connections %s -> %s)"
      % (time.time() - t0, total, s.get("connections_total")))
cmds_before = len(mock("/mock/state").get("commands", []))
mock("/mock/print?state=printing")  # a print starts while nobody looks; somebody is subscribed (webhook)
t0 = time.time()
while time.time() - t0 < 60 and mock("/mock/state").get("open") != 1:
    time.sleep(1)
check(mock("/mock/state").get("open") == 1, "the next check-in finds the print (%.0f s)" % (time.time() - t0))
time.sleep(25)  # well past KEEP (10 s here)
check(mock("/mock/state").get("open") == 1, "the print is held while it runs (somebody is subscribed)")
cmds = mock("/mock/state").get("commands", [])[cmds_before:]
check(not any(c[0] == 324 for c in cmds), "no CANVAS read while nobody looks")
mock("/mock/print?state=complete")
t0 = time.time()
while time.time() - t0 < 45 and mock("/mock/state").get("open") != 0:
    time.sleep(1)
check(mock("/mock/state").get("open") == 0, "closed about KEEP after the print ended (%.0f s)" % (time.time() - t0))
hooks = mock("/mock/state").get("hooks", 0)
check(hooks > 0, "the subscribed destination got the print's notifications (%s)" % hooks)

print("\n%d checks, %d failed" % (checks, len(fails)))
for f in fails:
    print("  FAILED: " + f)
sys.exit(1 if fails else 0)

"""Gate for the hub's Elegoo Centauri Carbon 2 backend (ElegooCc2Monitor, owner D8), against a running test
instance and a private printer emulator (tools/printer_emulator, personality elegoo_cc2) it starts itself.

The instance must have been started on a data dir whose hub/print_host_devices.json lists ONE device: an
Elegoo Centauri Carbon 2 at --host (id cc2gate01, empty API key = the default access code), with
SNORCA_CC2_KEEP_S=10 and SNORCA_CC2_CHECKIN_S=30 (gate_hubcc2.sh sets this up). --host must be a loopback
address with ports 80 and 1883 free. Covers:
  * GET /api/printers: the ph: row has the state, temperatures, fans, light, the CANVAS (Combo) as `ams`
    and the `cc2` details; mid-print: progress, layers, time left and the job name move on;
  * /summary carries the same through to the app;
  * pause (1021), resume (1023), stop (1022, needs confirm=1) reach the printer over the same MQTT
    connection, and the row follows; a temperature / light / fan is refused (read-only);
  * one MQTT client for all of it, closed after KEEP with nobody asking;
  * a printer that goes off: an offline row with the reason and no buttons; back again after it returns;
  * the event watcher sees the print start and end (GET /api/events).
Nothing is ever sent to a real printer: the only printer is the emulator on a loopback address.

usage: test_hub_cc2_gate.py --datadir=<the instance's data dir> --host=127.0.0.x [--token=testtoken12345]
"""
import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "emulator"))
from emu_proc import EmulatorProcess, assert_loopback  # noqa: E402
from emu_assert import Emulator, EmuAssertionError  # noqa: E402

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
DATADIR = opts.get("--datadir")
HOST = opts.get("--host")
TOKEN = opts.get("--token", "testtoken12345")
PID_ID = "ph:cc2gate01"
EMU_ID = "gate-cc2combo"
if not DATADIR or not HOST:
    raise SystemExit(__doc__)
assert_loopback(HOST)
fails = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)
    return cond


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
if not check(bool(live), "hub %s lists a live instance" % HUB):
    sys.exit(1)
API = "%s/r/%s/i/%d/api" % (HUB, TOKEN, live[0]["pid"])


def api(path, method="GET", body=None, timeout=60):
    return http(API + path, method, body, timeout=timeout)


def row():
    st, pl = api("/printers")
    for p in pl.get("printers", []):
        if p.get("id") == PID_ID:
            return p
    return {}


def until(pred, timeout=25):
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


def expect(emu, what, **kw):
    try:
        emu.expect(EMU_ID, **kw)
        return check(True, what)
    except EmuAssertionError as e:
        return check(False, "%s\n%s" % (what, str(e)[:1500]))


def heater(p, hid):
    for h in (p.get("controls") or {}).get("heaters", []):
        if h.get("id") == hid:
            return h
    return {}


def edgeslicer_clients(emu):
    """The MQTT clients EdgeSlicer has open on the emulated printer (1_PC_<n>)."""
    snap = emu.state(EMU_ID)
    return [c for c in (snap.get("mqtt_clients") or []) if str(c).startswith("1_PC_")]


printer = {"id": EMU_ID, "personality": "elegoo_cc2", "name": "Gate CC2 Combo", "bind": HOST, "port": 80, "state": "standby",
           "access_code": "123456", "canvas": {"attached": True, "active_tray": 1, "auto_refill": False},
           "slots": [{"type": "PLA", "brand": "ELEGOO", "color": "#D02A2A", "loaded": True},
                     {"type": "PLA", "brand": "ELEGOO", "color": "#F5F5F5", "loaded": True},
                     {"type": "PETG", "brand": "ELEGOO", "color": "#202020", "loaded": True}, {"loaded": False}],
           "faults": {}}


def main():
    emu_p = EmulatorProcess(tempfile.mkdtemp(prefix="gate_hubcc2_emu_"), [printer])
    emu_p.start()
    emu = Emulator(emu_p.control)
    try:
        run(emu)
    finally:
        emu_p.stop()
    print("\n== CC2 hub gate: %d checks, %d failed ==" % (checks, len(fails)), flush=True)
    for f in fails:
        print("  FAILED: " + f)
    return 1 if fails else 0


def run(emu):
    print("-- idle CC2 Combo --", flush=True)
    p = until(lambda p: p.get("status") == "standby" and bool(p.get("ams")), 40)
    check(p.get("status") == "standby", "the CC2 row says standby (%s; %s)" % (p.get("status"), p.get("status_error")))
    check(p.get("online") is True and p.get("cc2_connected") is True, "online, over the hub's own MQTT connection")
    cc2 = p.get("cc2") or {}
    check(cc2.get("protocol") == "mqtt" and cc2.get("live") is True and cc2.get("firmware") == "emu", "cc2 details: mqtt, live, firmware (%s)" % cc2)
    check(abs(heater(p, "bed").get("temp", 0) - 25.0) < 0.01 and heater(p, "bed").get("settable") is False, "the bed reading, read-only")
    check(heater(p, "nozzle0").get("temp") == 26.0 and heater(p, "chamber").get("temp") == 24.0, "nozzle and chamber readings")
    check(((p.get("controls") or {}).get("light") or {}).get("on") is True, "the light")
    ams = p.get("ams") or []
    check(len(ams) == 1 and ams[0].get("name") == "CANVAS 1" and len(ams[0].get("trays", [])) == 4, "the CANVAS as one ams unit of four trays")
    if ams:
        trays = ams[0]["trays"]
        check(trays[1].get("active") is True and trays[2].get("type") == "PETG" and trays[3].get("exists") is False,
              "the tray in the toolhead, the PETG tray and the empty one")
    check(cc2.get("canvas_attached") is True and cc2.get("auto_refill") is False, "canvas_attached, auto_refill off")
    check(p.get("can_pause") is False and p.get("can_stop") is False, "no buttons while idle")
    check(len(edgeslicer_clients(emu)) == 1, "one MQTT client from EdgeSlicer (%s)" % edgeslicer_clients(emu))

    print("-- printing --", flush=True)
    mark = emu.mark()
    emu.set_state(EMU_ID, "printing")
    p = until(lambda p: p.get("status") == "printing" and (p.get("percent") or 0) >= 1, 30)
    check(p.get("status") == "printing" and p.get("printing") is True, "the row follows the push: printing")
    check(p.get("task") == "emu_job.gcode", "the job name (%s)" % p.get("task"))
    first = p.get("percent", 0)
    p = until(lambda q: (q.get("percent") or 0) >= first + 2, 20)
    check((p.get("percent") or 0) >= first + 2, "progress moves on mid-print (%s -> %s)" % (first, p.get("percent")))
    check(p.get("total_layers") == 120 and (p.get("layer") or 0) >= 1, "layers (%s / %s)" % (p.get("layer"), p.get("total_layers")))
    check((p.get("left_time_s") or 0) > 0, "time left (%s)" % p.get("left_time_s"))
    check(heater(p, "nozzle0").get("target") == 220, "the nozzle target while printing")
    check(p.get("can_pause") is True and p.get("can_stop") is True and p.get("can_resume") is False, "pause and stop offered")

    print("-- /summary --", flush=True)
    srow = {}
    for _ in range(20):
        st, s = http("%s/r/%s/summary" % (HUB, TOKEN))
        srow = next((r for r in s.get("printers", []) if r.get("id") == PID_ID), {})
        if srow.get("state") == "printing":
            break
        time.sleep(1)
    check(srow.get("state") == "printing", "/summary state printing")
    check(srow.get("percent", 0) > 0 and srow.get("job") == "emu_job.gcode", "/summary percent and job")
    check(bool(srow.get("ams")) and srow["ams"][0].get("name") == "CANVAS 1", "/summary carries the CANVAS")
    check(bool(srow.get("controls")) and bool(srow.get("cc2")), "/summary carries controls and cc2")
    check(isinstance(srow.get("chamber_temp"), (int, float)), "/summary carries the chamber reading")

    print("-- controls --", flush=True)
    st, j = control(action="set_temp", heater="bed", target="60")
    check(st == 409, "set_temp is refused (read-only): %d %s" % (st, j.get("error")))
    st, j = control(action="set_light", on="0")
    check(st == 409, "set_light is refused (read-only): %d" % st)
    st, j = control(action="stop")
    check(st == 400, "stop without confirm=1 is refused (%d)" % st)
    st, j = control(action="pause")
    check(st == 200 and j.get("job"), "pause accepted (%d %s)" % (st, j))
    if j.get("job"):
        r = wait_job(j["job"])
        check(r.get("state") == "done", "the pause job finishes (%s %s)" % (r.get("state"), r.get("error")))
    expect(emu, "the emulator got 1021 pause", kind="control", since=mark, action="pause", error_code=0)
    p = until(lambda p: p.get("status") == "paused", 15)
    check(p.get("status") == "paused" and p.get("can_resume") is True, "the row says paused, resume offered")
    st, j = control(action="pause")
    check(st == 409, "pausing a paused printer is refused (%d)" % st)
    st, j = control(action="resume")
    if j.get("job"):
        wait_job(j["job"])
    expect(emu, "the emulator got 1023 resume", kind="control", since=mark, action="resume", error_code=0)
    p = until(lambda p: p.get("status") == "printing", 15)
    check(p.get("status") == "printing", "printing again")
    st, j = control(action="stop", confirm="1")
    if j.get("job"):
        wait_job(j["job"])
    expect(emu, "the emulator got 1022 stop", kind="control", since=mark, action="stop", error_code=0)
    p = until(lambda p: p.get("status") in ("cancelled", "standby"), 15)
    check(p.get("status") in ("cancelled", "standby") and p.get("printing") is False, "the row says cancelled / standby (%s)" % p.get("status"))
    check(len(edgeslicer_clients(emu)) == 1, "still one MQTT client for everything (%s)" % edgeslicer_clients(emu))

    print("-- the event watcher --", flush=True)
    # The watcher polls on its own cadence (RemoteEvents): give it a while to report the print.
    seen, st, t0 = [], 0, time.time()
    while time.time() - t0 < 60 and not seen:
        st, ev = api("/events?limit=50")
        seen = [e for e in ev.get("events", []) if PID_ID in json.dumps(e)]
        if not seen:
            time.sleep(3)
    check(st == 200, "GET /api/events answers (%d)" % st)
    check(bool(seen), "the event watcher saw the CC2's print (%d events: %s)" % (len(seen), [e.get("type") for e in seen]))

    print("-- nobody asking: the connection closes after KEEP --", flush=True)
    # The hub's own poll counts as somebody looking for 45 s after the last /summary or proxied
    # /api/printers (RemoteHub m_watch_ms), then KEEP (10 s here) runs out.
    t0 = time.time()
    while time.time() - t0 < 100 and edgeslicer_clients(emu):
        time.sleep(2)
    check(not edgeslicer_clients(emu), "no MQTT client left after KEEP with nobody looking (%d s)" % (time.time() - t0))

    print("-- the printer goes off, and comes back --", flush=True)
    emu._call("/emu/printers/%s/stop" % EMU_ID, {})
    p = until(lambda p: p.get("status") == "offline", 40)
    check(p.get("status") == "offline" and p.get("online") is False, "offline (%s)" % p.get("status"))
    check(bool(p.get("status_error")), "with the reason (%s)" % p.get("status_error"))
    check(p.get("can_pause") is False and p.get("can_stop") is False and "percent" not in p, "no buttons and no job while offline")
    st, j = control(action="pause")
    check(st == 409, "pause on an offline printer is refused (%d)" % st)
    emu._call("/emu/printers/%s/start" % EMU_ID, {})
    p = until(lambda p: p.get("online") is True and p.get("status") == "standby", 60)
    check(p.get("online") is True and p.get("status") == "standby", "back again (%s)" % p.get("status"))


if __name__ == "__main__":
    sys.exit(main())

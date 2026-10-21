"""Self-test of the printer emulator alone (no EdgeSlicer): a private emulator on a free loopback
address, driven over HTTP the way EdgeSlicer drives a QIDI, checked through emu_assert.

    python tests/emulator/test_emulator_selftest.py [--work <scratch dir>]

Exit 0 when every check passes. Takes about 20 s.
"""
import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_proc import EmulatorProcess, free_port  # noqa: E402
from emu_assert import Emulator, EmuAssertionError  # noqa: E402

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
WORK = opts.get("--work") or tempfile.mkdtemp(prefix="emu_selftest_")
HOST = "127.0.0.%d" % (60 + os.getpid() % 100)
PORT = free_port(HOST)
PID = "selftest-qidi"
fails, checks = [], 0


def check(cond, what):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)


def http(path, method="GET", body=None, ctype=None, port=PORT, timeout=15):
    req = urllib.request.Request("http://%s:%d%s" % (HOST, port, path), data=body, method=method,
                                 headers={"Content-Type": ctype} if ctype else {})
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except OSError as e:
        return 0, str(e).encode()


def script(text):
    return http("/printer/gcode/script?script=" + urllib.parse.quote(text), "POST", b"{}", "application/json")


def variables():
    st, raw = http("/printer/objects/query?save_variables=variables")
    return json.loads(raw)["result"]["status"]["save_variables"]["variables"]


def upload(name, data, print_=True, path="/api/files/local", port=PORT):
    b = "emuBoundary7"
    body = ("--{b}\r\nContent-Disposition: form-data; name=\"print\"\r\n\r\n{p}\r\n"
            "--{b}\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n\r\n"
            "--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{n}\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n").format(b=b, p="true" if print_ else "false", n=name).encode()
    body += data + ("\r\n--%s--\r\n" % b).encode()
    return http(path, "POST", body, "multipart/form-data; boundary=" + b, port=port)


printer = {"id": PID, "personality": "qidi", "name": "EMU selftest", "bind": HOST, "port": PORT, "state": "standby",
           "qidi": {"box_report": "mcc+vars", "boxes": 1, "polar_cooler": True, "stale_value_t": {"0": "slot3"}},
           "slots": [{"type": "PLA", "color": "#E03030", "loaded": True}, {"type": "ASA", "color": "#202020", "loaded": True},
                     {"loaded": False}, {"type": "PETG", "color": "#FFFFFF", "loaded": True}], "faults": {}}
emu_p = EmulatorProcess(os.path.join(WORK, "emuhome"), [printer])
emu_p.start()
emu = Emulator(emu_p.control)
try:
    print("-- the probe --")
    mark = emu.reset(PID)
    st, raw = http("/printer/objects/query?print_stats&print_task_config&multi_color_controller&output_pin%20polar_cooler"
                   "&extruder&extruder1&save_variables=variables")
    s = json.loads(raw)["result"]["status"]
    check(st == 200 and s["print_stats"]["state"] == "standby", "probe: standby")
    check("print_task_config" not in s and "extruder1" not in s, "objects the printer lacks are left out")
    check(s["multi_color_controller"]["box_count"] == 1 and s["multi_color_controller"]["boxes"][0]["box_inserted_2"] is False,
          "multi_color_controller: one Box, slot 3 empty")
    check(s["save_variables"]["variables"]["value_t0"] == "slot3", "the stale value_t0 is slot3")
    check("output_pin polar_cooler" in s, "polar cooler pin")
    emu.expect(PID, kind="probe", since=mark)

    print("-- SAVE_VARIABLE is interpreted --")
    st, raw = script("SAVE_VARIABLE VARIABLE=enable_box VALUE=1\nSAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot0'\"\n"
                     "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=1\n")
    check(st == 200 and json.loads(raw) == {"result": "ok"}, "script ok")
    v = variables()
    check(v["value_t0"] == "slot0" and v["enable_box"] == 1 and v["enable_polar_cooler"] == 1, "read-back returns what was stored")
    emu.expect(PID, kind="save_variable", var="value_t0", value="slot0", stored=True, since=mark)
    emu.expect(PID, kind="readback", since=mark)
    st, raw = script("SAVE_VARIABLE VARIABLE=value_t1 VALUE=slot2")
    check(st == 400 and b"Unable to parse" in raw, "an unquoted string is Klipper's literal error (%d)" % st)
    st, raw = script("SAVE_VARIABLE VARIABLE=Value_T1 VALUE=1")
    check(st == 400 and b"upper case" in raw, "upper-case variable names are refused")
    st, raw = script("SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot1'\"\nSAVE_VARIABLE VALUE=1\nSAVE_VARIABLE VARIABLE=value_t3 VALUE=\"'slot1'\"")
    v = variables()
    check(st == 400 and v.get("value_t2") == "slot1" and "value_t3" not in v, "the first failing line stops the script")

    print("-- faults: drop_save_variable, refuse_g31, refuse_script --")
    mark = emu.reset(PID, {"faults": {"drop_save_variable": True}})
    st, _ = script("SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot1'\"")
    check(st == 200, "drop_save_variable still answers ok")
    check(variables()["value_t0"] == "slot3", "... but the read-back still says slot3")
    emu.expect(PID, kind="save_variable", var="value_t0", stored=False, since=mark)
    emu.set(PID, {"faults": {"drop_save_variable": False, "refuse_g31": True}})
    st, raw = script("G31")
    check(st == 400 and b"G31 refused" in raw, "refuse_g31: Klipper error (%d)" % st)
    st, _ = script("G32")
    check(st == 200 and emu.state(PID)["bedmesh_before_print"] is False, "G32 still accepted: saved mesh")
    emu.set(PID, {"faults": {"refuse_g31": False, "refuse_script": True}})
    st, _ = script("G31")
    check(st == 400, "refuse_script refuses everything")
    emu.set(PID, {"faults": {"refuse_script": False}})

    print("-- upload and start --")
    mark = emu.reset(PID)
    st, raw = upload("plate_1.gcode", b"G28\nG1 X10\n" * 100)
    check(st == 201, "OctoPrint-compatible upload with print=true (%d)" % st)
    e = emu.expect(PID, kind="upload", name="plate_1.gcode", since=mark)
    check(e["payload"]["size"] == 1100 and len(e["payload"]["sha256"]) == 64, "name, size and sha256 recorded")
    emu.expect(PID, kind="start", file="plate_1.gcode", since=mark)
    emu.expect_order(PID, ["upload", "start"], since=mark)
    check(emu.state(PID)["print_state"] == "printing", "printing after the start")
    st, _ = upload("plate_2.gcode", b"G28\n", path="/server/files/upload")
    check(st == 201, "a second upload is stored while printing")
    emu.expect(PID, kind="start", file="plate_2.gcode", refused=True, since=mark)
    st, _ = http("/printer/print/start?filename=nothere.gcode", "POST", b"{}", "application/json")
    check(st in (400, 404), "a start for a file never uploaded is refused (%d)" % st)

    print("-- timelapse, filament list --")
    st, raw = http("/machine/timelapse/settings?enabled=true", "POST", b"{}", "application/json")
    check(st == 200 and json.loads(raw)["result"]["enabled"] is True, "timelapse setting stored")
    st, raw = http("/server/files/config/officiall_filas_list.cfg")
    check(st == 200 and b"[colordict]" in raw and b"type = ASA" in raw, "synthetic officiall_filas_list.cfg")
    emu.set(PID, {"faults": {"filas_cfg_404": True, "timelapse_404": True}})
    check(http("/server/files/config/officiall_filas_list.cfg")[0] == 404, "filas_cfg_404")
    check(http("/machine/timelapse/settings?enabled=true", "POST", b"{}", "application/json")[0] == 404, "timelapse_404")
    emu.set(PID, {"faults": {"filas_cfg_404": False, "timelapse_404": False}})

    print("-- Box variants --")
    emu.reset(PID, {"qidi": {"box_report": "vars_only"}})
    s = json.loads(http("/printer/objects/query?multi_color_controller&save_variables=variables")[1])["result"]["status"]
    check(s["multi_color_controller"] == {"box_count": 1}, "vars_only: the controller counts the Box, names nothing")
    check(s["save_variables"]["variables"]["filament_slot1"] > 0, "vars_only: filament_slot1 set")
    emu.reset(PID, {"qidi": {"box_report": "mcc+vars"}, "faults": {"box_unplugged": True}})
    s = json.loads(http("/printer/objects/query?multi_color_controller&save_variables=variables")[1])["result"]["status"]
    check(s["multi_color_controller"]["box_count"] == 0 and s["save_variables"]["variables"]["box_count"] == 0, "box_unplugged")
    emu.reset(PID, {"faults": {"box_unplugged": False}})

    print("-- slow probe, timeout, connection refused, http_error, 8080-only upload --")
    emu.set(PID, {"faults": {"probe_delay_s": 3}})
    t0 = time.time()
    http("/printer/objects/query?print_stats")
    check(time.time() - t0 >= 2.8, "probe_delay_s delays the query (%.1f s)" % (time.time() - t0))
    emu.set(PID, {"faults": {"probe_delay_s": 0, "timeout": True}})
    t0 = time.time()
    st, _ = http("/server/info", timeout=2)
    check(st == 0 and time.time() - t0 >= 1.8, "timeout: no answer")
    emu.set(PID, {"faults": {"timeout": False, "refuse_connections": True}})
    st, raw = http("/server/info", timeout=3)
    check(st == 0, "refuse_connections: nothing listens (%s)" % raw[:60])
    emu.set(PID, {"faults": {"refuse_connections": False, "http_error": {"/server/info": 503}}})
    check(http("/server/info")[0] == 503, "http_error per path")
    emu.set(PID, {"faults": {"http_error": {}}})
    p8080_free = True
    try:
        emu.set(PID, {"faults": {"upload_8080_only": True}})
        p = emu.printer(PID)
        p8080_free = any(a.endswith(":8080") for a in p["addresses"])
    except EmuAssertionError:
        p8080_free = False
    if p8080_free:
        emu.reset(PID)
        st, _ = upload("p8080.gcode", b"G28\n", path="/server/files/upload")
        check(st == 500, "upload_8080_only: the main port refuses the upload")
        st, _ = upload("p8080.gcode", b"G28\n", path="/server/files/upload", port=8080)
        check(st == 201, "... and %s:8080 takes it" % HOST)
    else:
        print("  skip 8080 (port taken on %s)" % HOST)
    emu.set(PID, {"faults": {"upload_8080_only": False}})

    print("-- safety: a public or wildcard bind is refused --")
    for bad in ("0.0.0.0", "8.8.8.8", "printer.local"):
        try:
            emu.set(PID, {"bind": bad})
            check(False, "bind %s refused" % bad)
        except EmuAssertionError as e:
            check("HTTP 400" in str(e), "bind %s refused" % bad)
    try:
        emu.set(PID, {"bind": "8.8.8.8", "lan": True})
        check(False, "lan bind to a public address refused")
    except EmuAssertionError:
        check(True, "lan bind to a public address refused")

    print("-- assertions explain themselves --")
    try:
        emu.expect(PID, kind="save_variable", var="value_t9", since=0)
        check(False, "expect raises")
    except EmuAssertionError as e:
        check("log slice" in str(e), "a failed expect prints the log slice")
finally:
    emu_p.stop()

print("\n%d checks, %d failed" % (checks, len(fails)))
for f in fails:
    print("  FAILED: " + f)
sys.exit(1 if fails else 0)

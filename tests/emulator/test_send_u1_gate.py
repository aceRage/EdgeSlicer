"""Snapmaker U1 send gate (design 5, the U1 matrix): EdgeSlicer's hub LAN send path
(POST /api/plates/0/send to an sm: printer, SnapmakerLan) against the emulated U1.

An installed EdgeSlicer is started hidden on its own scratch data dir whose hub/snapmaker_lan.json
lists ONE U1 at a 127.0.0.x address served by the emulator. A two-filament PLA plate (red on
filament 1, white on filament 2, made with the CLI) is sliced through the hub API and sent once per
row; each row asserts on the emulator's command log.

  U1  toolheads red PLA, white PLA, yellow PETG,      upload print=false, then SET_PRINT_EXTRUDER_MAP 0->0,
      empty; auto mapping                             1->1, SET_PRINT_USED_EXTRUDERS 0,1, SET_PRINT_PREFERENCES,
                                                      then the start; the printer prints
  U2  mapping=0:1,1:0                                 the extruder map follows the request (0->1, 1->0)
  U3  unload_at_end=1                                 SET_PRINT_PREFERENCES ... END_UNLOAD_FILAMENT=[1,1,0,0]
  U4  the printer is printing                         refused before anything is sent (no upload, no script)
  U5  a filament mapped onto the empty toolhead       refused (409) before anything is sent
  U6  the printer refuses SET_PRINT_EXTRUDER_MAP      job error; the file is on the printer, no start

The firmware's own refusal of SET_PRINT_PREFERENCES while printing (the end-of-print unload) is
covered by test_emulator_e2_selftest.py and the Catch2 [.EmulatorE2E] run: the hub never gets that
far with a busy printer (U4).

usage: python tests/emulator/test_send_u1_gate.py [--exe=C:/Dev/EdgeSlicerBuilds/current/EdgeSlicer.exe]
           [--work=<scratch dir>] [--template=<a data dir with the first-run wizard done>]
"""
import hashlib
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from emu_proc import EmulatorProcess, assert_loopback, free_port  # noqa: E402
from emu_assert import Emulator  # noqa: E402
import gate_common as g  # noqa: E402

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
EXE = opts.get("--exe", g.DEFAULT_EXE)
WORK = os.path.abspath(opts.get("--work") or tempfile.mkdtemp(prefix="emu_u1_gate_"))
TEMPLATE = opts.get("--template") or g.DEFAULT_TEMPLATE
PRINTER_ID = "gate-u1"
SERIAL = "EMU-U1-GATEU1"   # what the emulator reports for id gate-u1
MODEL, VENDOR = "Snapmaker U1", "Snapmaker"
PRESET = "Snapmaker U1 (0.4 nozzle)"
PROCESS = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)"
FILAMENT = "Generic PLA @U1 0.4 nozzle"
RED, WHITE = "#D93A2B", "#F5F5F5"
HOST = assert_loopback("127.0.0.%d" % (160 + os.getpid() % 40))
PORT = free_port(HOST)
DD = os.path.join(WORK, "dd")
FIXTURE = os.path.join(WORK, "u1_2pla.3mf")
L = g.Ledger("U1 send gate")

SLOTS = [{"type": "PLA", "sub": "Matte", "vendor": "Snapmaker", "color": RED, "loaded": True},
         {"type": "PLA", "sub": "Basic", "vendor": "Snapmaker", "color": WHITE, "loaded": True},
         {"type": "PETG", "sub": "Basic", "vendor": "Snapmaker", "color": "#F5F04A", "loaded": True},
         {"type": "TPU", "color": "#000000", "loaded": False}]
BASE = {"state": "standby", "slots": SLOTS, "faults": {"refuse_extruder_map": False, "start_error_state": False}}


def setup(emu, **changes):
    patch = json.loads(json.dumps(BASE))
    patch.update(changes)
    mark = emu.reset(PRINTER_ID, patch)
    # The hub keeps a U1's probe for SnapmakerLan::Presence::PROBE_TTL_MS (4 s): let the reading it
    # has of the previous row go stale before the next send asks.
    time.sleep(5)
    return emu.mark() if mark is None else mark


def nothing_sent(emu, mark, label):
    for kind in ("upload", "extruder_map", "used_extruders", "preferences", "script", "start"):
        g.expect_none(L, emu, PRINTER_ID, "%s: no %s" % (label, kind), kind, mark)


def run_matrix(inst, emu):
    printer = "sm:" + SERIAL

    L.row("U1", "auto mapping by colour")
    mark = setup(emu)
    j = inst.send(printer)
    L.check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    up = g.expect(L, emu, PRINTER_ID, "upload with print=false", "upload", mark, print=False)
    src = (j.get("result") or {}).get("source") or ""
    if up and os.path.isfile(src):
        h = hashlib.sha256(open(src, "rb").read()).hexdigest()
        L.check(up["payload"]["sha256"] == h, "the uploaded bytes are the plate's G-code (sha256 %s...)" % h[:12])
    g.expect(L, emu, PRINTER_ID, "SET_PRINT_EXTRUDER_MAP 0 -> 0 (red)", "extruder_map", mark, config=0, map=0)
    g.expect(L, emu, PRINTER_ID, "SET_PRINT_EXTRUDER_MAP 1 -> 1 (white)", "extruder_map", mark, config=1, map=1)
    g.expect(L, emu, PRINTER_ID, "SET_PRINT_USED_EXTRUDERS 0,1", "used_extruders", mark, extruders=[0, 1])
    pref = g.expect(L, emu, PRINTER_ID, "SET_PRINT_PREFERENCES sent", "preferences", mark)
    if pref:
        print("  note preferences %s" % {k: v for k, v in pref["payload"].items() if k != "line"})
    g.expect_order(L, emu, PRINTER_ID, "order: upload, map, used, preferences, start",
                   ["upload", "extruder_map", "used_extruders", "preferences", "start"], mark)
    L.check(emu.state(PRINTER_ID)["print_state"] == "printing", "the printer is printing")
    r = (j.get("result") or {})
    L.check(r.get("printer_state") == "printing", "the hub saw the printer printing (%s)" % r.get("printer_state"))

    L.row("U2", "explicit mapping 0:1,1:0")
    mark = setup(emu)
    j = inst.send(printer, mapping="0:1,1:0")
    L.check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    g.expect(L, emu, PRINTER_ID, "SET_PRINT_EXTRUDER_MAP 0 -> 1", "extruder_map", mark, config=0, map=1)
    g.expect(L, emu, PRINTER_ID, "SET_PRINT_EXTRUDER_MAP 1 -> 0", "extruder_map", mark, config=1, map=0)
    g.expect(L, emu, PRINTER_ID, "SET_PRINT_USED_EXTRUDERS 1,0", "used_extruders", mark, extruders=[1, 0])
    st = emu.state(PRINTER_ID)
    L.check(st["extruder_map"][:2] == [1, 0], "print_task_config.extruder_map_table now starts 1, 0 (%s)" % st["extruder_map"][:4])

    L.row("U3", "unload at the end of the print")
    mark = setup(emu)
    j = inst.send(printer, unload_at_end="1")
    L.check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    g.expect(L, emu, PRINTER_ID, "END_UNLOAD_FILAMENT=[1,1,0,0] on SET_PRINT_PREFERENCES", "preferences", mark,
             end_unload_filament=[1, 1, 0, 0])

    L.row("U4", "refused while printing")
    mark = setup(emu, state="printing")
    j = inst.send(printer)
    L.check(j.get("state") == "refused" and "printing" in (j.get("error") or ""), "refused: %s (HTTP %s)" % (j.get("error"), j.get("http")))
    nothing_sent(emu, mark, "printing")

    L.row("U5", "a filament mapped onto the empty toolhead")
    mark = setup(emu)
    j = inst.send(printer, mapping="0:3,1:1")
    L.check(j.get("state") == "refused" and "empty" in (j.get("error") or ""), "refused: %s (HTTP %s)" % (j.get("error"), j.get("http")))
    nothing_sent(emu, mark, "empty toolhead")

    L.row("U6", "the printer refuses SET_PRINT_EXTRUDER_MAP")
    mark = setup(emu, faults={"refuse_extruder_map": True, "start_error_state": False})
    j = inst.send(printer)
    L.check(j.get("state") == "error" and "mapping" in (j.get("error") or ""), "job error: %s" % j.get("error"))
    g.expect(L, emu, PRINTER_ID, "the file was uploaded first (print=false)", "upload", mark, print=False)
    g.expect(L, emu, PRINTER_ID, "the map was refused", "extruder_map", mark, refused=True)
    g.expect_none(L, emu, PRINTER_ID, "no start", "start", mark)
    L.check(emu.state(PRINTER_ID)["print_state"] == "standby", "the printer is still idle")


def main():
    os.makedirs(WORK, exist_ok=True)
    if not os.path.isfile(EXE):
        raise SystemExit("no EdgeSlicer.exe at %s" % EXE)
    print("gate work dir %s\nexe %s\nemulated U1 at %s:%d (loopback)" % (WORK, EXE, HOST, PORT), flush=True)
    g.make_fixture(EXE, WORK, FIXTURE, PRESET, PROCESS, [FILAMENT, FILAMENT], [RED, WHITE])
    lan = {"devices": [{"id": SERIAL, "name": "EMU gate U1", "model": MODEL, "ip": HOST, "port": PORT, "added_by": "manual"}]}
    opened = g.make_datadir(TEMPLATE, DD, FIXTURE, MODEL, VENDOR, snapmaker_lan=lan)
    printer = {"id": PRINTER_ID, "personality": "u1", "name": "EMU gate U1", "bind": HOST, "port": PORT}
    printer.update(json.loads(json.dumps(BASE)))
    emu_p = EmulatorProcess(os.path.join(WORK, "emuhome"), [printer])
    inst = g.Instance(EXE, DD, FIXTURE)
    try:
        emu_p.start()
        emu = Emulator(emu_p.control)
        inst.start()
        L.row("setup", "open, slice, U1 listed")
        inst.open_and_slice(L, opened, PRESET)
        st, pl = inst.call("/printers?plate=0")
        dev = next((p for p in pl.get("printers", []) if p.get("id") == "sm:" + SERIAL), {})
        L.check(bool(dev), "sm:%s listed (%s)" % (SERIAL, {k: dev.get(k) for k in ("state", "url", "ip", "online", "model")}))
        if not L.fails:
            run_matrix(inst, emu)
    finally:
        inst.stop()
        emu_p.stop()
        print("(work dir with the instance, emulator and CLI logs: %s)" % WORK)
    return L.summary()


if __name__ == "__main__":
    sys.exit(main())

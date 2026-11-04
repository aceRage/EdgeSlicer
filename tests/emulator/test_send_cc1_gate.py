"""Elegoo Centauri Carbon 1 send gate (design 5, the CC1 matrix): EdgeSlicer's hub send path
(POST /api/plates/0/send) against the emulated CC1 with a CANVAS.

An installed EdgeSlicer is started hidden on its own scratch data dir with ONE print-host device, an
Elegoo Centauri Carbon (host type Elegoo Link) at a 127.0.0.x address of its own - the CC1's ports
80 / 3030 / 3031 are fixed in EdgeSlicer, so the gate picks an address where all three are free. A
two-filament PLA plate (red on filament 1, white on filament 2, made with the CLI) is sliced
through the hub API and sent once per row; each row asserts on the emulator's command log.

  C1  CANVAS attached, PLA trays, no mapping asked   chunked upload (MD5 good), Cmd 128 Ack 0 on the
                                                      uploaded file; slot_map = the desktop dialog's
                                                      automatic choice (closest colour): T0 -> tray 0,
                                                      T1 -> tray 1
  C2  the same, with a mapping (T0 -> tray 1,         Cmd 128 carries slot_map [{t,canvas_id,tray_id}...]
      T1 -> tray 0)
  C3  CANVAS detached                                 Cmd 128 without slot_map
  C4  PLA plate, every tray ASA                       refused, naming the filament and the tray ("Filament 1
                                                      is PLA but tray 1A holds ASA"); nothing uploaded,
                                                      no Cmd 128
  C4u the same, upload only                           the file is uploaded, no Cmd 128 (upload stays possible)
  C5  upload MD5 mismatch                             job error naming the MD5 check; no Cmd 128
  C6  the printer is printing                         refused as "busy" BEFORE anything is uploaded; no
                                                      print started
  C7  viewer limit reached (busy)                     the start websocket is refused; job error, no Cmd 128

C1, C2, C4 and C6 need the product fix of PR #660 (fix/remote-send-slot-policy: the hub's
CC1 send reads the printer and the CANVAS, maps the trays the desktop dialog's way and refuses a
material mismatch or a busy printer before the upload). Against an older build they FAIL.

usage: python tests/emulator/test_send_cc1_gate.py [--exe=C:/Dev/EdgeSlicerBuilds/current/EdgeSlicer.exe]
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
from emu_proc import EmulatorProcess, assert_loopback  # noqa: E402
from emu_assert import Emulator  # noqa: E402
import gate_common as g  # noqa: E402

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
EXE = opts.get("--exe", g.DEFAULT_EXE)
WORK = os.path.abspath(opts.get("--work") or tempfile.mkdtemp(prefix="emu_cc1_gate_"))
TEMPLATE = opts.get("--template") or g.DEFAULT_TEMPLATE
PRINTER_ID = "gate-cc1"
DEVICE = "emucc101"
MODEL, VENDOR = "Elegoo Centauri Carbon", "Elegoo"
PRESET = "Elegoo Centauri Carbon 0.4 nozzle"
PROCESS = "0.20mm Standard @Elegoo CC 0.4 nozzle"
FILAMENT = "Elegoo PLA @ECC"
RED, WHITE = "#D02A2A", "#F5F5F5"
DD = os.path.join(WORK, "dd")
FIXTURE = os.path.join(WORK, "cc1_2pla.3mf")


def pick_host():
    from core import safety
    for n in range(110 + os.getpid() % 40, 150):
        h = "127.0.0.%d" % n
        if all(safety.port_free(h, p) for p in (80, 3030, 3031)):
            return assert_loopback(h)
    raise SystemExit("no 127.0.0.x with ports 80, 3030 and 3031 free")


HOST = pick_host()
L = g.Ledger("CC1 send gate")

SLOTS_PLA = [{"type": "PLA", "name": "ELEGOO PLA", "brand": "ELEGOO", "color": RED, "loaded": True},
             {"type": "PLA", "name": "PLA PRO", "color": WHITE, "loaded": True, "untagged": True},
             {"type": "PETG", "brand": "ELEGOO", "color": "#2050C0", "loaded": True}, {"loaded": False}]
SLOTS_ASA = [{"type": "ASA", "brand": "ELEGOO", "color": RED, "loaded": True},
             {"type": "ASA", "brand": "ELEGOO", "color": WHITE, "loaded": True}, {"loaded": False}, {"loaded": False}]
BASE = {"state": "standby", "canvas": {"attached": True, "active_tray": 0, "tray_id_base": 0}, "slots": SLOTS_PLA,
        "camera": {"enabled": True, "viewer_limit": 4},
        "faults": {"upload_md5_mismatch": False, "refuse_start": 0, "viewers_full": False, "ws_refuse": False}}


def setup(emu, **changes):
    patch = json.loads(json.dumps(BASE))
    for k, v in changes.items():
        if isinstance(v, dict) and isinstance(patch.get(k), dict):
            patch[k].update(v)
        else:
            patch[k] = v
    return emu.reset(PRINTER_ID, patch)


def no_start(emu, mark, label):
    starts = emu.find(PRINTER_ID, kind="start", since=mark)
    L.check(not [e for e in starts if not e["payload"].get("refused")], "%s: no print started (%d Cmd 128)" % (label, len(starts)))


def run_matrix(inst, emu):
    printer = "ph:" + DEVICE

    L.row("C1", "CANVAS attached, PLA trays, no mapping asked (the dialog's automatic choice)")
    mark = setup(emu)
    j = inst.send(printer)
    L.check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    up = g.expect(L, emu, PRINTER_ID, "chunked upload complete", "upload", mark)
    if up:
        L.check(not up["payload"].get("refused") and up["payload"]["md5"] == up["payload"]["md5_claimed"],
                "the printer's MD5 check passed (%s)" % up["payload"]["md5"])
    src = (j.get("result") or {}).get("source") or ""
    if up and os.path.isfile(src):
        data = open(src, "rb").read()
        L.check(up["payload"]["md5"] == hashlib.md5(data).hexdigest() and up["payload"]["size"] == len(data),
                "the uploaded bytes are the plate's G-code (%d bytes, %d chunks)" % (len(data), up["payload"]["chunks"]))
    st = g.expect(L, emu, PRINTER_ID, "Cmd 128 Ack 0", "start", mark, ack=0)
    if st and up:
        L.check(st["payload"]["file"] == up["payload"]["name"] and st["payload"]["filename"] == "/local/" + up["payload"]["name"],
                "Cmd 128 names the uploaded file (/local/%s)" % up["payload"]["name"])
        L.check(st["payload"]["slot_map"] == [{"t": 0, "canvas_id": 0, "tray_id": 0}, {"t": 1, "canvas_id": 0, "tray_id": 1}],
                "slot_map is the closest-colour match, red -> tray 0, white -> tray 1 (got %s: %s)"
                % (st["payload"]["slot_map_form"], st["payload"]["slot_map"]))
    g.expect_order(L, emu, PRINTER_ID, "order: upload, start", ["upload", "start"], mark)
    L.check(emu.state(PRINTER_ID)["print_state"] == "printing", "the printer is printing")

    L.row("C2", "CANVAS mapping T0->tray 1, T1->tray 0")
    mark = setup(emu)
    j = inst.send(printer, mapping="0:1,1:0")
    L.check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    st = g.expect(L, emu, PRINTER_ID, "Cmd 128 sent", "start", mark)
    if st:
        L.check(st["payload"]["slot_map"] == [{"t": 0, "canvas_id": 0, "tray_id": 1}, {"t": 1, "canvas_id": 0, "tray_id": 0}],
                "slot_map carries the mapping (got %s: %s)" % (st["payload"]["slot_map_form"], st["payload"]["slot_map"]))

    L.row("C3", "CANVAS detached")
    mark = setup(emu, canvas={"attached": False})
    j = inst.send(printer)
    L.check(j.get("state") == "done", "the send job is done (%s %s)" % (j.get("state"), j.get("error", "")))
    st = g.expect(L, emu, PRINTER_ID, "Cmd 128 Ack 0", "start", mark, ack=0)
    if st:
        L.check(st["payload"]["slot_map_form"] == "absent", "no slot_map key without a CANVAS (%s)" % st["payload"]["slot_map_form"])

    L.row("C4", "PLA plate, every CANVAS tray ASA")
    mark = setup(emu, slots=SLOTS_ASA)
    j = inst.send(printer)
    asked = j.get("state") == "refused" and j.get("http") in (400, 409)
    L.check(asked or j.get("state") == "error", "the send asks first or refuses (%s %s %s)" % (j.get("state"), j.get("http"), j.get("error", "")))
    err = j.get("error") or ""
    L.check("Filament 1 is PLA but tray 1A holds ASA" in err and "upload only" in err,
            "the refusal names the filament, the tray and the way out: %s" % err.replace("\n", " | "))
    g.expect_none(L, emu, PRINTER_ID, "nothing uploaded", "upload", mark)
    no_start(emu, mark, "ASA trays")

    L.row("C4u", "PLA plate, every CANVAS tray ASA, upload only")
    mark = setup(emu, slots=SLOTS_ASA)
    j = inst.send(printer, mode="upload")
    L.check(j.get("state") == "done", "the upload-only send is done (%s %s)" % (j.get("state"), j.get("error", "")))
    g.expect(L, emu, PRINTER_ID, "the file is uploaded", "upload", mark)
    g.expect_none(L, emu, PRINTER_ID, "no Cmd 128", "start", mark)

    L.row("C5", "upload MD5 mismatch")
    mark = setup(emu, faults={"upload_md5_mismatch": True})
    j = inst.send(printer)
    L.check(j.get("state") == "error", "the send job fails (%s)" % j.get("state"))
    L.check("MD5" in (j.get("error") or ""), "the error names the MD5 check: %s" % (j.get("error") or "").replace("\n", " | "))
    g.expect(L, emu, PRINTER_ID, "the upload was refused on the MD5", "upload", mark, refused=True)
    g.expect_none(L, emu, PRINTER_ID, "no Cmd 128", "start", mark)

    L.row("C6", "start refused: the printer is printing")
    mark = setup(emu, state="printing")
    j = inst.send(printer)
    L.check(j.get("state") in ("error", "refused") and "busy" in (j.get("error") or "").lower(),
            "refused as busy: %s" % (j.get("error") or "").replace("\n", " | "))
    for e in emu.find(PRINTER_ID, kind="start", since=mark):
        L.check(e["payload"]["ack"] == 1, "Cmd 128 answered Ack 1 (busy)")
    g.expect_none(L, emu, PRINTER_ID, "nothing was uploaded to the busy printer", "upload", mark)
    L.check(emu.state(PRINTER_ID)["current_file"] == "emu_job.gcode", "the printer's own job was not replaced")

    L.row("C7", "viewer limit reached (busy)")
    mark = setup(emu, faults={"viewers_full": True})
    j = inst.send(printer)
    L.check(j.get("state") == "error", "the send job fails (%s): %s" % (j.get("state"), (j.get("error") or "").replace("\n", " | ")))
    g.expect(L, emu, PRINTER_ID, "the start websocket was refused", "ws_refused", mark, fault="viewers_full")
    g.expect_none(L, emu, PRINTER_ID, "no Cmd 128", "start", mark)
    L.check(emu.state(PRINTER_ID)["print_state"] == "standby", "the printer is still idle")


def main():
    os.makedirs(WORK, exist_ok=True)
    if not os.path.isfile(EXE):
        raise SystemExit("no EdgeSlicer.exe at %s" % EXE)
    print("gate work dir %s\nexe %s\nemulated CC1 at %s (ports 80, 3030, 3031; loopback)" % (WORK, EXE, HOST), flush=True)
    g.make_fixture(EXE, WORK, FIXTURE, PRESET, PROCESS, [FILAMENT, FILAMENT], [RED, WHITE])
    devices = {"version": 1, "models": {MODEL: {"current": DEVICE, "migrated": [], "devices": [{
        "id": DEVICE, "alias": "EMU CC1", "address": HOST, "host_type": "elegoolink", "auth_type": "key", "apikey": "",
        "user": "", "password": "", "printer_model": MODEL, "created": 1760000000, "last_used": 0}]}}}
    opened = g.make_datadir(TEMPLATE, DD, FIXTURE, MODEL, VENDOR, print_host_devices=devices)
    printer = {"id": PRINTER_ID, "personality": "elegoo_cc1", "name": "EMU gate CC1", "bind": HOST, "port": 80}
    printer.update(json.loads(json.dumps(BASE)))
    emu_p = EmulatorProcess(os.path.join(WORK, "emuhome"), [printer])
    inst = g.Instance(EXE, DD, FIXTURE)
    try:
        emu_p.start()
        emu = Emulator(emu_p.control)
        inst.start()
        L.row("setup", "open, slice, device listed")
        inst.open_and_slice(L, opened, PRESET)
        st, pl = inst.call("/printers?plate=0")
        dev = next((p for p in pl.get("printers", []) if p.get("id") == "ph:" + DEVICE), {})
        L.check(dev.get("can_print") is True and assert_loopback(dev.get("url") or HOST),
                "ph:%s listed with can_print (%s)" % (DEVICE, {k: dev.get(k) for k in ("can_print", "url", "model", "state")}))
        if not L.fails:
            run_matrix(inst, emu)
    finally:
        inst.stop()
        emu_p.stop()
        print("(work dir with the instance, emulator and CLI logs: %s)" % WORK)
    return L.summary()


if __name__ == "__main__":
    sys.exit(main())

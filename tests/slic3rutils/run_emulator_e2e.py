"""Runs EdgeSlicer's printer-protocol calls end to end against the printer emulator
(tools/printer_emulator): PrintHostDevices::probe and query_qidi_variables on a QIDI X-Max 4,
canvas_query and the CANVAS probe on an Elegoo CC1, SnapmakerLan::status_now and run_script on a
Snapmaker U1.

    python run_emulator_e2e.py --exe <build>/tests/slic3rutils/slic3rutils_tests.exe

Copied from run_elegoo_sdcp_e2e.py: starts a private emulator (scratch home, its own control port,
loopback addresses only - asserted) with the three printers, runs the hidden Catch2 tag
[.EmulatorE2E] with them in EMU_CONTROL / EMU_QIDI / EMU_CC1 / EMU_U1, and always stops the emulator
again (by the handle this script started it with). No real printer is contacted. Exit code: the
test executable's.
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "emulator"))
from emu_proc import EmulatorProcess, assert_loopback, free_port  # noqa: E402


def cc1_host():
    """A 127.0.0.x with the CC1's fixed ports (80, 3030, 3031) free."""
    for n in range(230 + os.getpid() % 20, 254):
        h = "127.0.0.%d" % n
        ok = True
        for p in (80, 3030, 3031):
            s = socket.socket()
            try:
                s.bind((h, p))
            except OSError:
                ok = False
            finally:
                s.close()
        if ok:
            return assert_loopback(h)
    raise SystemExit("no 127.0.0.x with 80, 3030 and 3031 free")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="slic3rutils_tests.exe")
    ap.add_argument("--work", help="scratch folder for the emulator (default: a new temp folder, kept for its logs)")
    args, rest = ap.parse_known_args()
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        raise SystemExit("no test executable at " + exe)
    work = args.work or tempfile.mkdtemp(prefix="emu_e2e_")
    qidi_host = assert_loopback("127.0.0.%d" % (210 + os.getpid() % 20))
    u1_host = assert_loopback("127.0.0.%d" % (190 + os.getpid() % 20))
    qidi_port, u1_port = free_port(qidi_host), free_port(u1_host)
    cc1 = cc1_host()
    printers = [
        {"id": "e2e-qidi", "personality": "qidi", "name": "EMU e2e QIDI X-Max 4", "bind": qidi_host, "port": qidi_port,
         "state": "standby", "qidi": {"box_report": "mcc+vars", "boxes": 1, "polar_cooler": True,
                                      "stale_value_t": {"0": "slot3", "1": "slot2"}},
         "slots": [{"type": "PLA", "sub": "PLA", "color": "#E03030", "loaded": True},
                   {"type": "ASA", "sub": "ASA", "color": "#202020", "loaded": True}, {"loaded": False},
                   {"type": "PETG", "sub": "PETG", "color": "#FFFFFF", "loaded": True}], "faults": {}},
        {"id": "e2e-cc1", "personality": "elegoo_cc1", "name": "EMU e2e CC1", "bind": cc1, "port": 80, "state": "standby",
         "canvas": {"attached": True, "active_tray": 0},
         "slots": [{"type": "PLA", "brand": "ELEGOO", "color": "#D02A2A", "loaded": True},
                   {"type": "PLA", "name": "PLA PRO", "color": "#F5F5F5", "loaded": True, "untagged": True},
                   {"type": "ASA", "brand": "ELEGOO", "color": "#202020", "loaded": True}, {"loaded": False}],
         "camera": {"enabled": True, "viewer_limit": 4}, "faults": {}},
        {"id": "e2e-u1", "personality": "u1", "name": "EMU e2e U1", "bind": u1_host, "port": u1_port, "state": "standby",
         "slots": [{"type": "PLA", "color": "#D93A2B", "loaded": True}, {"type": "PLA", "color": "#F5F5F5", "loaded": True},
                   {"type": "PETG", "color": "#F5F04A", "loaded": True}, {"type": "TPU", "color": "#000000", "loaded": False}],
         "faults": {}},
    ]
    emu = EmulatorProcess(os.path.join(work, "emuhome"), printers, control_port=free_port())
    try:
        emu.start()
        env = dict(os.environ, EMU_CONTROL=emu.control, EMU_QIDI="%s:%d" % (qidi_host, qidi_port), EMU_CC1=cc1,
                   EMU_U1="%s:%d" % (u1_host, u1_port))
        env.pop("SNORCA_SDCP_PORT", None)
        print("emulator %s: QIDI %s, CC1 %s, U1 %s" % (emu.control, env["EMU_QIDI"], cc1, env["EMU_U1"]), flush=True)
        t0 = time.time()
        rc = subprocess.call([exe, "[EmulatorE2E]", "--durations", "yes"] + rest, env=env)
        print("EmulatorE2E exit %d after %.1f s" % (rc, time.time() - t0))
        return rc
    finally:
        emu.stop()
        print("(emulator home with its command log: %s)" % os.path.join(work, "emuhome"))


if __name__ == "__main__":
    sys.exit(main())

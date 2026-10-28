"""The emulator's control page lists the printers while printers are being added (owner's hand test,
2026-10-26: "RuntimeError: dictionary changed size during iteration" at core/control.py, do_GET).
Every reader now iterates Supervisor.printer_list(), a snapshot taken under the supervisor's lock.

Deterministic, in process: a printer is added while the listing is in the middle of describing the
printers (what a second thread does to the old code), and the listing must still finish.

    python tests/emulator/test_emulator_printer_list.py

Exit 0 when it passes. A few seconds; only loopback ports.
"""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_proc import free_port  # noqa: E402  (also puts tools/printer_emulator on sys.path)

from core import printer as printer_mod  # noqa: E402
from core.supervisor import Supervisor  # noqa: E402


def main():
    home = tempfile.mkdtemp(prefix="emu_printer_list_")
    cfg = os.path.join(home, "printers.json")
    with open(cfg, "w", encoding="utf-8") as f:
        json.dump({"printers": []}, f)
    sup = Supervisor(home, config_path=cfg, save=False)
    fails = []
    try:
        for i in range(2):
            sup.add(preset="moonraker_generic", overrides={"id": "base-%d" % i, "bind": "127.0.0.1", "port": free_port()})
        original = printer_mod.EmulatedPrinter.describe
        state = {"added": 0}

        def describe_and_add(self):
            # The first describe of a listing adds a printer, as the control page's poll racing an
            # "Add printer" click on another thread does.
            if state["added"] == 0:
                state["added"] = 1
                sup.add(preset="moonraker_generic", overrides={"id": "late", "bind": "127.0.0.1", "port": free_port()})
            return original(self)

        printer_mod.EmulatedPrinter.describe = describe_and_add
        try:
            listed = [x.describe() for x in sup.printer_list()]
            ok = len(listed) == 2 and state["added"] == 1
        except RuntimeError as e:
            ok = False
            fails.append(str(e))
        finally:
            printer_mod.EmulatedPrinter.describe = original
        print(("  ok   " if ok else "  FAIL ") + "the listing finishes while a printer is added (%s)" % (fails or "snapshot of 2"))
        if not ok and not fails:
            fails.append("unexpected listing")
        after = len(sup.printer_list())
        print(("  ok   " if after == 3 else "  FAIL ") + "the added printer is there afterwards (%d)" % after)
        if after != 3:
            fails.append("count %d" % after)
    finally:
        sup.stop_all()
    print("\n== emulator printer listing: %s ==" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

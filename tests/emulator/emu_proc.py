"""Start and stop a private emulator for a gate or a self-test: its own scratch home (never the
owner's %LOCALAPPDATA%\\EdgeSlicerEmulator), its own control port, its own printers.json, and
only loopback addresses (asserted here, before anything starts)."""
import ipaddress
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EMU_DIR = os.path.join(REPO, "tools", "printer_emulator")
if EMU_DIR not in sys.path:
    sys.path.insert(0, EMU_DIR)


def assert_loopback(address):
    """'127.0.0.61:17125' or '127.0.0.61' -> raises unless it is a loopback IP."""
    host = address.rsplit(":", 1)[0] if address.count(":") == 1 else address
    host = host.replace("http://", "").replace("https://", "").strip("[]/")
    if not ipaddress.ip_address(host).is_loopback:
        raise SystemExit("REFUSED: %s is not a loopback address; a gate never points at a real printer" % address)
    return address


def free_port(host="127.0.0.1"):
    s = socket.socket()
    s.bind((host, 0))
    p = s.getsockname()[1]
    s.close()
    return p


class EmulatorProcess:
    def __init__(self, home, printers, control_port=None, log_path=None):
        for p in printers:
            assert_loopback(p["bind"])
            if p.get("lan"):
                raise SystemExit("REFUSED: a gate printer must not opt in to a LAN bind")
        self.home = home
        self.printers = printers
        self.control_port = control_port or free_port()
        self.control = "http://127.0.0.1:%d" % self.control_port
        self.log_path = log_path or os.path.join(home, "emulator_stdout.log")
        self.proc = None

    def start(self, timeout=20):
        os.makedirs(self.home, exist_ok=True)
        cfg = os.path.join(self.home, "printers.json")
        with open(cfg, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"printers": self.printers}, f, indent=2)
        out = open(self.log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen([sys.executable, os.path.join(EMU_DIR, "emu.py"), "serve", "--home", self.home,
                                      "--config", cfg, "--control-port", str(self.control_port), "--no-browser"],
                                     stdout=out, stderr=subprocess.STDOUT, cwd=EMU_DIR)
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                r = json.loads(urllib.request.urlopen(self.control + "/emu/printers", timeout=2).read())
                bad = [p for p in r["printers"] if not p["running"] or p["error"]]
                if bad:
                    raise SystemExit("emulator printer did not start: %s" % [(p["id"], p["error"]) for p in bad])
                return r
            except (OSError, ValueError):
                if self.proc.poll() is not None:
                    raise SystemExit("the emulator exited: see %s" % self.log_path)
                time.sleep(0.3)
        raise SystemExit("the emulator did not come up in %d s (%s)" % (timeout, self.log_path))

    def stop(self):
        if not self.proc:
            return
        try:
            req = urllib.request.Request(self.control + "/emu/quit", data=b"{}", method="POST",
                                         headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5).read()
            self.proc.wait(10)
        except Exception:  # noqa: BLE001
            pass
        if self.proc.poll() is None:
            self.proc.kill()  # our own child, by handle
        self.proc = None

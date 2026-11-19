"""A stand-in Moonraker/Klipper printer for the phone-control gate (test_phone_control.py).

Speaks just enough of Moonraker's HTTP API for RemoteControl:
  GET  /printer/objects/query?print_stats   what the printer is doing (state, filename, message)
  POST /printer/print/pause | resume | cancel   the three controls; each records what it received
  GET  /server/info, /printer/info, /api/version   so a print-host preset can point at it
and its own control surface, so the gate can drive it:
  GET  /mock/state                 {state, filename, commands: [{path, t}], refuse}
  POST /mock/reset                 back to standby, commands cleared
  POST /mock/set?state=printing&filename=x.gcode&refuse=0|1
Nothing is ever printed: a pause only flips this mock's own reported state. `refuse=1` makes the
three control endpoints answer 400 with a Moonraker-shaped error, which is how the gate exercises
the refusal path without a printer.

usage: mock_moonraker.py <port> (runs until killed)
"""
import json, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18091
LOCK = threading.Lock()
STATE = {"state": "printing", "filename": "gate_job.gcode", "message": "", "refuse": False, "delay": 0.0, "commands": []}
ACTIONS = {"pause": "paused", "resume": "printing", "cancel": "cancelled"}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):  # quiet
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _drain(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/printer/objects/query":
            with LOCK:
                st = dict(STATE)
            return self._json(200, {"result": {"eventtime": time.time(), "status": {"print_stats": {
                "state": st["state"],
                "filename": st["filename"],
                "message": st["message"],
                "print_duration": 120.0,
                "total_duration": 130.0,
                "info": {"current_layer": 7, "total_layer": 40},
            }}}})
        if u.path == "/server/info":
            return self._json(200, {"result": {"klippy_connected": True, "klippy_state": "ready", "moonraker_version": "mock"}})
        if u.path == "/printer/info":
            return self._json(200, {"result": {"state": "ready", "hostname": "mock-u1", "software_version": "mock"}})
        if u.path == "/api/version":
            return self._json(200, {"api": "0.1", "server": "1.5.0", "text": "OctoPrint (Moonraker mock)"})
        if u.path == "/access/info":
            return self._json(200, {"result": {"login_required": False, "trusted": True}})
        if u.path == "/mock/state":
            with LOCK:
                return self._json(200, dict(STATE))
        return self._json(404, {"error": "no such route"})

    def do_POST(self):
        self._drain()
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/mock/reset":
            with LOCK:
                STATE.update({"state": "printing", "filename": "gate_job.gcode", "message": "", "refuse": False, "delay": 0.0, "commands": []})
                return self._json(200, dict(STATE))
        if u.path == "/mock/set":
            with LOCK:
                if "state" in q:
                    STATE["state"] = q["state"][0]
                if "filename" in q:
                    STATE["filename"] = q["filename"][0]
                if "message" in q:
                    STATE["message"] = q["message"][0]
                if "refuse" in q:
                    STATE["refuse"] = q["refuse"][0] in ("1", "true")
                if "delay" in q:
                    STATE["delay"] = float(q["delay"][0])
                return self._json(200, dict(STATE))
        if u.path.startswith("/printer/print/"):
            action = u.path.rsplit("/", 1)[1]
            if action not in ACTIONS:
                return self._json(404, {"error": "no such route"})
            with LOCK:
                STATE["commands"].append({"path": u.path, "action": action, "t": time.time()})
                refuse, delay = STATE["refuse"], STATE["delay"]
                if not refuse:
                    STATE["state"] = ACTIONS[action]
            if delay:  # a slow printer, so the gate can catch a second command in flight
                time.sleep(delay)
            if refuse:
                return self._json(400, {"error": {"code": 400, "message": "Print %s refused by the mock" % action}})
            return self._json(200, {"result": "ok"})
        return self._json(404, {"error": "no such route"})


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print("mock moonraker on http://127.0.0.1:%d" % PORT, flush=True)
    srv.serve_forever()

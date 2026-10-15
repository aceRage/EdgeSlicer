"""A fake Snapmaker U1 (Moonraker over HTTP) on 127.0.0.1, for testing the hub's LAN probe
(SnapmakerLan) end to end without a printer.

It answers what SnapmakerLan asks a U1:
  GET /server/info            {"result": {"klippy_state": "ready", ...}}
  GET /access/info            {"result": {"login_required": false, "trusted": true}}
  GET /printer/objects/query  print_stats, display_status, virtual_sdcard, heater_bed, extruder..extruder3 and
                              print_task_config, shaped like firmware 1.5.2's answer
  GET /machine/system_info    model, serial number and device name (what identify() reads)
  GET /printer/info           hostname
  POST /server/files/upload   multipart (root=gcodes, file=...): kept in memory, name -> bytes
  GET /server/files/metadata  ?filename= -> {size, modified, slicer numbers, one 32x32 thumbnail} of an
                              uploaded file, the slicer's numbers for fake_job.gcode (FAKE_JOB_META), 404 otherwise
  GET /server/files/list      ?root=gcodes -> every uploaded file
  GET /server/files/gcodes/<dir>/.thumbs/<stem>-32x32.png -> a 1x1 PNG for each file (the Storage tab)
  DELETE /server/files/gcodes/<path> -> deletes that file (404 when there is none)
  POST /printer/print/start   ?filename= -> starts that file (404 when it was never uploaded)
  POST /printer/gcode/script  ?script= -> accepted (the mapped start's macros)

and has a control port of its own, so a test can change how the "printer" behaves:
  GET /fake/mode?m=normal           answer at once
  GET /fake/mode?m=slow&delay=6     answer every printer request after `delay` seconds
  GET /fake/mode?m=down             stop listening on the printer port (connection refused)
  GET /fake/mode?m=up               listen again and answer at once (same as normal)
  GET /fake/state?state=printing    what print_stats.state says (standby, printing, paused, ...)
  GET /fake/stats                   {"mode":..., "requests": {path: count}}
  GET /fake/files                   {"files": {name: size}, "started": [names], "uploads": n, "deleted": [names]}
  GET /fake/add?name=a.gcode&size=n put a file there without an upload
  GET /fake/reset                   forget every file and start, state standby

Nothing here ever talks to a real printer: both ports bind to 127.0.0.1 only.

usage: fake_moonraker.py --port 18201 --control 18202 [--name U1-fake] [--sn FAKE0001]
       (runs until killed; prints "READY <port> <control>" once both ports listen)
"""
import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

LOCK = threading.Lock()
STATE = {"mode": "normal", "delay": 0.0, "state": "printing", "requests": {},
         "files": {}, "started": [], "uploads": 0, "current": "", "deleted": []}
ARGS = None
# server.files.metadata for the job a fresh fake prints (fake_job.gcode): body 100000..900000 of a
# 1 MB file and a 3600 s slicer estimate, so at file_position 300000 the hub should say 25 % and
# 2700 s left (the old linear guess from display_status 0.30 and print_duration 600 was 1400 s).
FAKE_JOB_META = {"filename": "fake_job.gcode", "size": 1000000, "modified": 1760000000.0, "slicer": "EdgeSlicer",
                 "gcode_start_byte": 100000, "gcode_end_byte": 900000, "estimated_time": 3600, "layer_count": 80}


# The smallest valid PNG (1x1, transparent): the picture every fake thumbnail serves.
TINY_PNG = bytes.fromhex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
                         "0000000d4944415478da636460f85f0f0002870180eb47ba920000000049454e44ae426082")


def reply_bytes(handler, code, data, content_type):
    try:
        handler.send_response(code)
        handler.send_header("Content-Type", content_type)
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)
    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
        pass


def reply(handler, code, obj):
    body = json.dumps(obj).encode()
    try:
        handler.send_response(code)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
        pass  # the client gave up waiting (a timeout test): nothing to do


def query_status(state):
    temps = [(215.0, 220.0), (30.5, 0.0), (29.8, 0.0), (30.1, 0.0)]
    status = {
        "print_stats": {
            "state": state,
            "filename": (STATE["current"] or "fake_job.gcode") if state in ("printing", "paused") else "",
            "message": "",
            "print_duration": 600.0,
            "total_duration": 640.0,
            "info": {"current_layer": 12, "total_layer": 80},
        },
        # The share of the whole file (thumbnails included), as Klipper reports it; the hub's
        # percent is the share of the G-code body instead (FAKE_JOB_META), like the Device page's.
        "display_status": {"progress": 0.30 if state in ("printing", "paused") else 0.0},
        "virtual_sdcard": {
            "file_path": ("/home/pi/printer_data/gcodes/" + (STATE["current"] or "fake_job.gcode")) if state in ("printing", "paused") else None,
            "progress": 0.30 if state in ("printing", "paused") else 0.0,
            "is_active": state == "printing",
            "file_position": 300000 if state in ("printing", "paused") else 0,
            "file_size": FAKE_JOB_META["size"] if state in ("printing", "paused") else 0,
        },
        "heater_bed": {"temperature": 60.2, "target": 60.0},
        "print_task_config": {
            "filament_type": ["PLA", "PETG", "PLA", "TPU"],
            "filament_color_rgba": ["FF0000FF", "00FF00FF", "0000FFFF", "FFFFFFFF"],
            "filament_exist": [True, True, False, True],
        },
    }
    for i, (t, target) in enumerate(temps):
        status["extruder" if i == 0 else "extruder%d" % i] = {
            "temperature": t, "target": target, "nozzle_diameter": 0.4}
    return {"result": {"eventtime": time.time(), "status": status}}


class Printer(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        pass

    def do_GET(self):
        u = urlparse(self.path)
        with LOCK:
            STATE["requests"][u.path] = STATE["requests"].get(u.path, 0) + 1
            mode, delay, state = STATE["mode"], STATE["delay"], STATE["state"]
        if mode == "slow" and delay > 0:
            time.sleep(delay)
        if u.path == "/server/info":
            return reply(self, 200, {"result": {"klippy_connected": True, "klippy_state": "ready",
                                                "moonraker_version": "fake"}})
        if u.path == "/access/info":
            return reply(self, 200, {"result": {"login_required": False, "trusted": True}})
        if u.path == "/printer/objects/query":
            return reply(self, 200, query_status(state))
        if u.path == "/machine/system_info":
            return reply(self, 200, {"result": {"system_info": {"product_info": {
                "machine_type": "Snapmaker U1", "serial_number": ARGS.sn, "device_name": ARGS.name}}}})
        if u.path == "/printer/info":
            return reply(self, 200, {"result": {"hostname": ARGS.name, "state": "ready"}})
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/server/files/metadata":
            with LOCK:
                size = STATE["files"].get(q.get("filename", ""))
            if size is None and q.get("filename", "") == "fake_job.gcode":
                return reply(self, 200, {"result": FAKE_JOB_META})
            if size is None:
                return reply(self, 404, {"error": {"code": 404, "message": "Metadata not available for " + q.get("filename", "")}})
            name = q["filename"]
            stem = name.rsplit("/", 1)[-1].rsplit(".", 1)[0]
            # What a slicer writes, plus one thumbnail beside the file (the Storage tab's picture),
            # relative to the file's own directory as Moonraker reports it.
            return reply(self, 200, {"result": {"filename": name, "size": size, "modified": time.time(),
                                                "slicer": "EdgeSlicer", "estimated_time": 4980,
                                                "filament_total": 4560.0, "filament_weight_total": 13.6,
                                                "filament_type": "PLA", "layer_height": 0.2,
                                                "thumbnails": [{"width": 32, "height": 32, "size": len(TINY_PNG),
                                                                "relative_path": ".thumbs/" + stem + "-32x32.png"}]}})
        if u.path.startswith("/server/files/gcodes/"):
            # Only the thumbnails are served: the fake keeps sizes, not the files' bytes.
            path = unquote(u.path[len("/server/files/gcodes/"):])
            with LOCK:
                names = list(STATE["files"])
            for n in names:
                d, _, base = n.rpartition("/")
                thumb = (d + "/" if d else "") + ".thumbs/" + base.rsplit(".", 1)[0] + "-32x32.png"
                if path == thumb:
                    return reply_bytes(self, 200, TINY_PNG, "image/png")
            return reply(self, 404, {"error": {"code": 404, "message": "File not found: " + path}})
        if u.path == "/server/files/list":
            with LOCK:
                files = [{"path": n, "size": sz, "modified": time.time(), "permissions": "rw"} for n, sz in STATE["files"].items()]
            return reply(self, 200, {"result": files})
        return reply(self, 404, {"error": {"code": 404, "message": "Not Found"}})

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        with LOCK:
            STATE["requests"][u.path] = STATE["requests"].get(u.path, 0) + 1
        if u.path == "/server/files/upload":
            name, data = multipart_file(self.headers.get("Content-Type", ""), body)
            if not name:
                return reply(self, 400, {"error": {"code": 400, "message": "no file in the upload"}})
            with LOCK:
                STATE["files"][name] = len(data)
                STATE["uploads"] += 1
            return reply(self, 201, {"result": {"item": {"path": name, "root": "gcodes"}, "action": "create_file"}})
        if u.path == "/printer/print/start":
            name = q.get("filename", "")
            with LOCK:
                if name not in STATE["files"]:
                    return reply(self, 404, {"error": {"code": 404, "message": "File not found: " + name}})
                STATE["started"].append(name)
                STATE["current"] = name
                STATE["state"] = "printing"
            return reply(self, 200, {"result": "ok"})
        if u.path == "/printer/gcode/script":
            return reply(self, 200, {"result": "ok"})
        return reply(self, 404, {"error": {"code": 404, "message": "Not Found"}})

    def do_DELETE(self):
        u = urlparse(self.path)
        with LOCK:
            STATE["requests"]["DELETE " + u.path] = STATE["requests"].get("DELETE " + u.path, 0) + 1
        if u.path.startswith("/server/files/gcodes/"):
            path = unquote(u.path[len("/server/files/gcodes/"):])
            with LOCK:
                if path not in STATE["files"]:
                    return reply(self, 404, {"error": {"code": 404, "message": "File does not exist: " + path}})
                del STATE["files"][path]
                STATE["deleted"].append(path)
            return reply(self, 200, {"result": {"item": {"path": path, "root": "gcodes"}, "action": "delete_file"}})
        return reply(self, 404, {"error": {"code": 404, "message": "Not Found"}})


def multipart_file(content_type, body):
    """The one file part of a multipart/form-data body: (filename, bytes)."""
    import email.parser, email.policy
    msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
        b"Content-Type: " + content_type.encode() + b"\r\n\r\n" + body)
    for part in msg.iter_parts():
        name = part.get_filename()
        if name:
            return name, part.get_payload(decode=True) or b""
    return "", b""


class PrinterPort:
    """The printer's listening socket, which `down` really closes and `up` opens again."""

    def __init__(self, port):
        self.port = port
        self.server = None
        self.thread = None

    def up(self):
        if self.server:
            return
        ThreadingHTTPServer.allow_reuse_address = True
        ThreadingHTTPServer.daemon_threads = True
        self.server = ThreadingHTTPServer(("127.0.0.1", self.port), Printer)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def down(self):
        if not self.server:
            return
        self.server.shutdown()
        self.server.server_close()
        self.server = None


PORT = None


class Control(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        pass

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/fake/mode":
            m = q.get("m", "normal")
            if m not in ("normal", "slow", "down", "up"):
                return reply(self, 400, {"error": "unknown mode " + m})
            with LOCK:
                STATE["mode"] = "normal" if m == "up" else m
                STATE["delay"] = float(q.get("delay", "6")) if m == "slow" else 0.0
            if m == "down":
                PORT.down()
            else:
                PORT.up()
            return reply(self, 200, {"mode": STATE["mode"], "delay": STATE["delay"]})
        if u.path == "/fake/state":
            with LOCK:
                STATE["state"] = q.get("state", "standby")
            return reply(self, 200, {"state": STATE["state"]})
        if u.path == "/fake/files":
            with LOCK:
                return reply(self, 200, {"files": dict(STATE["files"]), "started": list(STATE["started"]),
                                         "uploads": STATE["uploads"], "deleted": list(STATE["deleted"])})
        if u.path == "/fake/add":
            with LOCK:
                STATE["files"][q.get("name", "added.gcode")] = int(q.get("size", "1000"))
            return reply(self, 200, {"ok": True})
        if u.path == "/fake/reset":
            with LOCK:
                STATE.update({"files": {}, "started": [], "uploads": 0, "current": "", "state": "standby",
                              "deleted": []})
            return reply(self, 200, {"ok": True})
        if u.path == "/fake/stats":
            with LOCK:
                return reply(self, 200, {"mode": STATE["mode"], "delay": STATE["delay"],
                                         "requests": dict(STATE["requests"])})
        return reply(self, 404, {"error": "unknown"})


def main():
    global ARGS, PORT
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--control", type=int, required=True)
    ap.add_argument("--name", default="U1-fake")
    ap.add_argument("--sn", default="FAKE0001")
    ARGS = ap.parse_args()
    PORT = PrinterPort(ARGS.port)
    PORT.up()
    ctl = ThreadingHTTPServer(("127.0.0.1", ARGS.control), Control)
    print("READY %d %d" % (ARGS.port, ARGS.control), flush=True)
    try:
        ctl.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

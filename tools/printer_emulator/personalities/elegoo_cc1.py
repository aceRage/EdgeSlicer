"""Elegoo Centauri Carbon 1 with a CANVAS, speaking SDCP V3.0.0 (design 2.2).

Folded in from tests/phone/mock_elegoo_sdcp.py (frame shapes from the owner's capture, firmware
V1.4.49), plus what that mock lacked: the chunked HTTP upload, the start command (Cmd 128) with its
slot_map, settable CANVAS trays and a camera that shows a real picture.

Listeners, all on the printer's own address (the ports are fixed in EdgeSlicer, so every CC1 needs
its own 127.0.0.x):

  :<port> (80)  GET /                    a page that says ELEGOO (ElegooLink's connection test)
                GET /api/version         404, so the OctoPrint test fails and ElegooLink's own path runs
                POST /uploadFile/upload  chunked multipart: Check, S-File-MD5, Offset, Uuid, TotalSize,
                                         File; reassembled by Uuid, MD5-checked when complete
  :3030         the same HTTP, and ws://<ip>:3030/websocket:
                  "ping" -> "pong"
                  Cmd 0 status, Cmd 1 attributes, Cmd 324 CANVAS (Ack 0 + canvas_list, Ack 1 when
                  detached), Cmd 128 start print (logged with its full slot_map: present, [] or
                  absent), Cmd 129 / 131 / 130 pause / resume / stop, Cmd 386 camera URL
                  ("<ip>:3031/video", no scheme, as V1.4.49), Cmd 403 light and fans
                  status pushes every second while printing, and on every change
  :3031         GET /video   MJPEG: a test pattern with the printer's name and a clock

UNVERIFIED (no capture yet): the upload's error body for an MD5 mismatch ({"code": "...",
"messages": [{"field", "message"}]} is what ElegooLink parses, the code value is made up), the
Ack a real printer gives a slot_map naming an empty tray, and the SDCP status numbers for
"error". Everything is marked so in the log payloads ("unverified").
"""
import hashlib
import json
import re
import threading
import time

from core import jpeg, multipart, safety, ws
from core.http import Response
from .moonraker import fault_on

SDCP_PORT = 3030
CAMERA_PORT = 3031
UNTAGGED_BRAND = "— — — —"   # what V1.4.49 reports for an untagged / hand-entered spool
STATES = ("standby", "printing", "paused", "complete", "cancelled", "error")
BUSY = ("printing", "paused")
# print_state -> (CurrentStatus, PrintInfo.Status), from the mock (the owner's capture).
SDCP_STATE = {"standby": (0, 0), "printing": (1, 13), "paused": (1, 6), "complete": (0, 9), "cancelled": (0, 8),
              "error": (0, 0)}
ACK_TEXT = {0: "ok", 1: "busy", 2: "file not found", 3: "MD5 check failed", 4: "file I/O error",
            5: "invalid resolution", 6: "unknown format", 7: "file does not match the printer"}

FAULTS = [
    {"key": "timeout", "label": "Timeout (accept, never answer)", "type": "bool",
     "help": "Every HTTP request and websocket handshake is accepted and never answered."},
    {"key": "refuse_connections", "label": "Connection refused", "type": "bool",
     "help": "All three listeners closed, as a printer that is switched off."},
    {"key": "http_error", "label": "HTTP error per path", "type": "map", "help": "path=code, e.g. /uploadFile/upload=500"},
    {"key": "upload_md5_mismatch", "label": "Upload MD5 mismatch", "type": "bool",
     "help": "The reassembled file fails the printer's MD5 check: the last chunk answers an error, nothing is stored."},
    {"key": "upload_fail", "label": "Upload fails (HTTP 500)", "type": "bool", "help": ""},
    {"key": "refuse_start", "label": "Refuse Cmd 128 with Ack", "type": "number",
     "help": "0 = off; 1 busy, 2 file not found, 3 MD5 failed, 4 file I/O, 7 wrong model."},
    {"key": "viewers_full", "label": "Viewer limit reached (busy)", "type": "bool",
     "help": "Every websocket and camera slot is taken: new websocket handshakes get 403, camera viewers are closed."},
    {"key": "ws_refuse", "label": "Refuse websocket handshakes", "type": "bool", "help": ""},
    {"key": "mute", "label": "Websocket mute (answer nothing)", "type": "bool",
     "help": "Connects, then never answers a command or pushes a status: a printer that hangs."},
    {"key": "file_check_s", "label": "File check after upload (seconds)", "type": "number",
     "help": "CurrentStatus 8 for N s after an upload; ElegooLink waits for it before Cmd 128."},
]


def norm_colour(c):
    c = (c or "").strip().upper()
    if c.startswith("0X"):
        c = c[2:]
    if c and not c.startswith("#"):
        c = "#" + c
    return c[:7] if len(c) >= 7 else ""


FILAMENT_TYPE_RE = re.compile(rb"^;\s*filament_type\s*=\s*(.*?)\s*$", re.M)
FILAMENT_COLOUR_RE = re.compile(rb"^;\s*filament_colou?r\s*=\s*(.*?)\s*$", re.M)


def file_filaments(data):
    """The sliced file's filament types and colours, from the slicer's config block, so a
    slot_map entry can be compared with the tray it names. [] when the file does not say."""
    m = FILAMENT_TYPE_RE.findall(data)
    types = [t.strip() for t in m[-1].decode("utf-8", "replace").split(";")] if m else []
    c = FILAMENT_COLOUR_RE.findall(data)
    colours = [x.strip() for x in c[-1].decode("utf-8", "replace").split(";")] if c else []
    return [{"t": i, "type": t, "colour": colours[i] if i < len(colours) else ""} for i, t in enumerate(types)]


class ElegooCC1:
    name = "elegoo_cc1"
    label = "Elegoo Centauri Carbon 1 (SDCP, CANVAS)"
    host_type_hint = "Elegoo Link"
    FAULTS = FAULTS

    def __init__(self, printer):
        self.printer = printer
        self.clients = set()
        self.streams = 0
        self.stopping = threading.Event()
        self.pusher = None
        self.reset()

    # ---- config and state -------------------------------------------------------------------
    @property
    def cfg(self):
        return self.printer.cfg

    @property
    def faults(self):
        return self.cfg.get("faults") or {}

    @property
    def ccfg(self):
        return self.cfg.get("canvas") or {}

    def canvas_attached(self):
        return bool(self.ccfg.get("attached", True))

    def reset(self):
        st = self.cfg.get("state", "standby")
        self.print_state = st if st in STATES else "standby"
        self.current_file = ""
        self.files = {}
        self.chunks = {}          # Uuid -> {name, total, md5, parts {offset: bytes}}
        self.progress = 0
        self.layer = 0
        self.ticks = 0
        self.check_until = 0.0
        self.light = 1
        self.rgb = [255, 255, 255]
        self.fans = {"ModelFan": 0, "AuxiliaryFan": 0, "BoxFan": 0}
        self.last_start = None
        if self.print_state in BUSY:
            self.current_file = "emu_job.gcode"
            self.progress, self.layer, self.ticks = 17, 12, 600

    def config_changed(self, old):
        if self.cfg.get("state") != old.get("state") and self.cfg.get("state") in STATES:
            self.set_state(self.cfg["state"])
        elif (old.get("canvas") or {}) != self.ccfg or old.get("slots") != self.cfg.get("slots"):
            self.push_status()

    def set_state(self, state):
        self.print_state = state if state in STATES else "standby"
        if state in BUSY and not self.current_file:
            self.current_file = "emu_job.gcode"
        if state == "printing":
            self.progress, self.layer, self.ticks = max(self.progress, 1), max(self.layer, 1), max(self.ticks, 1)
        if state == "complete":
            self.progress = 100
        if state == "standby":
            self.progress, self.layer, self.ticks, self.current_file = 0, 0, 0, ""
        self.push_status()

    def busy(self):
        return self.print_state in BUSY

    def listen_ports(self):
        ports = [int(self.cfg.get("port", 80)), SDCP_PORT]
        if (self.cfg.get("camera") or {}).get("enabled", True):
            ports.append(CAMERA_PORT)
        return sorted(set(ports))

    def bind(self):
        return self.cfg.get("bind", "127.0.0.1")

    def mainboard_id(self):
        return "EMU-CC1-" + "".join(c for c in self.printer.id.upper() if c.isalnum())[:12]

    def viewer_limit(self):
        try:
            return max(0, int((self.cfg.get("camera") or {}).get("viewer_limit", 4)))
        except (TypeError, ValueError):
            return 4

    def snapshot(self):
        return {"print_state": self.print_state, "current_file": self.current_file, "files": sorted(self.files),
                "canvas_attached": self.canvas_attached(),
                "trays": [{"tray_id": t["tray_id"], "type": t["filament_type"], "colour": t["filament_color"],
                           "brand": t["brand"], "status": t["status"]} for t in self.trays()],
                "ws_clients": len(self.clients), "camera_streams": self.streams,
                "last_start": self.last_start, "progress": self.progress}

    def hint(self):
        port = int(self.cfg.get("port", 80))
        return {"host_type": self.host_type_hint,
                "address": self.bind() if port == 80 else "%s:%d" % (self.bind(), port),
                "printer_preset": self.cfg.get("edgeslicer_preset") or "Elegoo Centauri Carbon 0.4 nozzle",
                "camera": "http://%s:%d/video" % (self.bind(), CAMERA_PORT),
                "mac_alias": safety.mac_alias_command(self.bind())}

    # ---- the CANVAS -------------------------------------------------------------------------
    def slots(self):
        s = [dict(x) if isinstance(x, dict) else {} for x in (self.cfg.get("slots") or [])][:4]
        while len(s) < 4:
            s.append({"loaded": False})
        return s

    def trays(self):
        base = int(self.ccfg.get("tray_id_base", 0) or 0)
        active = self.ccfg.get("active_tray", -1)
        out = []
        for i, s in enumerate(self.slots()):
            loaded = bool(s.get("loaded")) and bool((s.get("type") or "").strip())
            untagged = bool(s.get("untagged"))
            t = {"tray_id": base + i, "brand": "", "filament_type": "", "filament_name": "", "filament_code": "0x00000",
                 "filament_color": "", "min_nozzle_temp": 0, "max_nozzle_temp": 0, "status": 0}
            if loaded:
                typ = s["type"].strip()
                t.update({
                    "brand": UNTAGGED_BRAND if untagged else (s.get("brand") or "ELEGOO"),
                    "filament_type": typ,
                    "filament_name": s.get("name") or (typ if untagged else "%s %s" % (s.get("brand") or "ELEGOO", typ)),
                    "filament_code": "0x00000" if untagged else (s.get("code") or "0x0001"),
                    "filament_color": norm_colour(s.get("color")) or "#FFFFFF",
                    "min_nozzle_temp": int(s.get("min_temp") or 190), "max_nozzle_temp": int(s.get("max_temp") or 230),
                    "status": 2 if active == i else 1})
            out.append(t)
        return out

    def canvas_payload(self):
        active = self.ccfg.get("active_tray", -1)
        base = int(self.ccfg.get("tray_id_base", 0) or 0)
        return {"Ack": 0, "active_canvas_id": 0, "active_tray_id": base + active if isinstance(active, int) and active >= 0 else -1,
                "auto_refill": int(self.ccfg.get("auto_refill", 0) or 0),
                "canvas_list": [{"canvas_id": 0, "connected": 1, "tray_list": self.trays()}]}

    # ---- SDCP frames ------------------------------------------------------------------------
    def status_frame(self):
        machine, pstat = SDCP_STATE.get(self.print_state, (0, 0))
        if time.time() < self.check_until:
            machine = 8
        printing = self.print_state in BUSY
        return {
            "Status": {
                "CurrentStatus": [machine], "PreviousStatus": 0,
                "TempOfNozzle": 220.0 if printing else 26.0, "TempTargetNozzle": 220 if printing else 0,
                "TempOfHotbed": 60.0 if printing else 24.0, "TempTargetHotbed": 60 if printing else 0,
                "TempOfBox": 25.0, "TempTargetBox": 0, "CurrenCoord": "120.00,110.00,3.20",
                "CurrentFanSpeed": dict(self.fans), "ZOffset": 0.0,
                "LightStatus": {"SecondLight": self.light, "RgbLight": list(self.rgb)},
                "TimeLapseStatus": 0, "AmsConnectStatus": 1 if self.canvas_attached() else 0,
                "PrintInfo": {"Status": pstat, "CurrentLayer": self.layer, "TotalLayer": 150 if self.current_file else 0,
                              "CurrentTicks": self.ticks, "TotalTicks": 3600 if self.current_file else 0,
                              "Progress": self.progress, "Filename": self.current_file,
                              "ErrorNumber": 1 if self.print_state == "error" else 0,
                              "TaskId": "emu-task-%04d" % len(self.files), "PrintSpeedPct": 100},
            },
            "MainboardID": self.mainboard_id(), "TimeStamp": int(time.time()), "Topic": "sdcp/status/" + self.mainboard_id(),
        }

    def attributes_frame(self):
        return {
            "Attributes": {
                "Name": self.cfg.get("name") or "EMU CC1", "MachineName": "Centauri Carbon", "BrandName": "ELEGOO",
                "ProtocolVersion": "V3.0.0", "FirmwareVersion": "V1.4.49-emu", "MainboardID": self.mainboard_id(),
                "Capabilities": ["FILE_TRANSFER", "PRINT_CONTROL", "VIDEO_STREAM"], "CameraStatus": 1,
                "MaximumVideoStreamAllowed": self.viewer_limit(), "NumberOfVideoStreamConnected": self.streams,
            },
            "MainboardID": self.mainboard_id(), "TimeStamp": int(time.time()), "Topic": "sdcp/attributes/" + self.mainboard_id(),
        }

    def response(self, cmd, rid, data):
        return {"Id": "", "Data": {"Cmd": cmd, "Data": data, "RequestID": rid, "MainboardID": self.mainboard_id(),
                                   "TimeStamp": int(time.time())}, "Topic": "sdcp/response/" + self.mainboard_id()}

    def broadcast(self, obj):
        text = json.dumps(obj)
        for c in list(self.clients):
            try:
                c.send_text(text)
            except OSError:
                pass

    def push_status(self):
        if self.clients and not fault_on(self.faults, "mute"):
            self.broadcast(self.status_frame())

    # ---- HTTP -------------------------------------------------------------------------------
    def route(self, req):
        p, m = req.path.rstrip("/") or "/", req.method
        if req.port == CAMERA_PORT:
            if m == "GET" and p == "/video":
                return self.camera(req)
            return Response(404, "not found", ctype="text/plain", kind="camera")
        if m in ("GET", "HEAD") and p in ("/", "/index.html"):
            return Response(200, "<!doctype html><html><head><title>ELEGOO</title></head><body>ELEGOO Centauri Carbon "
                                 "(EdgeSlicer printer emulator %s, %s)</body></html>" % (self.printer.id, self.mainboard_id()),
                            ctype="text/html", kind="info")
        if m == "POST" and p == "/uploadFile/upload":
            return self.upload_chunk(req)
        # /api/version and everything else: no OctoPrint here, so ElegooLink takes its own path.
        return Response(404, {"code": "404", "messages": [{"field": "path", "message": "not found"}], "success": False},
                        kind="http")

    def upload_chunk(self, req):
        fields, files = multipart.parse(req.headers.get("Content-Type", ""), req.body)
        if fault_on(self.faults, "upload_fail"):
            return Response(500, {"code": "500", "messages": [{"field": "common_field", "message": "emulated upload failure"}],
                                  "success": False}, kind="upload", payload={"refused": True, "fault": "upload_fail"})
        if not files:
            return Response(200, {"code": "100001", "messages": [{"field": "File", "message": "no file"}], "success": False},
                            kind="upload", payload={"refused": True, "error": "no file part", "fields": fields})
        _f, filename, data = files[0]
        uuid = fields.get("Uuid", "")
        try:
            offset, total = int(fields.get("Offset", "0")), int(fields.get("TotalSize", "0"))
        except ValueError:
            offset, total = 0, 0
        md5 = (fields.get("S-File-MD5") or "").lower()
        job = self.chunks.setdefault(uuid, {"name": filename, "total": total, "md5": md5, "parts": {}, "started": time.time()})
        job["parts"][offset] = data
        got = sum(len(v) for v in job["parts"].values())
        chunk = {"name": filename, "uuid": uuid, "offset": offset, "length": len(data), "total": total, "received": got,
                 "check": fields.get("Check"), "md5_claimed": md5}
        if got < total:
            return Response(200, {"code": "000000", "messages": None, "data": {"uuid": uuid, "offset": offset}, "success": True},
                            kind="upload_chunk", payload=chunk)
        # Complete: reassemble in offset order and check the MD5 the slicer sent.
        del self.chunks[uuid]
        whole = b"".join(job["parts"][o] for o in sorted(job["parts"]))
        actual = hashlib.md5(whole).hexdigest()
        mismatch = actual != md5 or fault_on(self.faults, "upload_md5_mismatch")
        info = {"name": filename, "size": len(whole), "md5": actual, "md5_claimed": md5, "chunks": len(job["parts"]),
                "sha256": hashlib.sha256(whole).hexdigest()}
        if mismatch:
            self.printer.sub_log(req, "upload_chunk", chunk)
            info.update({"refused": True, "error": "MD5 check failed",
                         "fault": "upload_md5_mismatch" if actual == md5 else None, "unverified": "error body shape"})
            return Response(200, {"code": "100003", "messages": [{"field": "S-File-MD5", "message": "MD5 check failed"}],
                                  "data": None, "success": False}, kind="upload", payload=info)
        stored = self.printer.store.put(self.printer.id, filename, whole)
        info["stored"] = stored.get("stored")
        info["filaments"] = file_filaments(whole)
        info["stored_at"] = stored["stored_at"]
        self.files[filename] = info
        self.printer.sub_log(req, "upload_chunk", chunk)
        try:
            check_s = float(self.faults.get("file_check_s") or 0)
        except (TypeError, ValueError):
            check_s = 0
        if check_s > 0:
            self.check_until = time.time() + check_s
        payload = {k: v for k, v in info.items() if k != "stored"}
        return Response(200, {"code": "000000", "messages": None, "data": {"uuid": uuid, "offset": offset, "complete": True},
                              "success": True}, kind="upload", payload=payload)

    # ---- the camera -------------------------------------------------------------------------
    def camera(self, req):
        if fault_on(self.faults, "viewers_full") or self.streams >= self.viewer_limit():
            return Response(0, None, kind="camera", drop=True,
                            payload={"refused": True, "why": "viewer limit (%d) reached" % self.viewer_limit(),
                                     "fault": "viewers_full" if fault_on(self.faults, "viewers_full") else None})
        self.streams += 1
        name = self.cfg.get("name") or self.printer.id
        stop = self.stopping

        def stream(wfile):
            try:
                frame, at = b"", -1
                while not stop.is_set():
                    now = int(time.time())
                    if now != at:
                        frame, at = jpeg.test_pattern(name, now), now
                    wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(frame) + frame + b"\r\n")
                    wfile.flush()
                    stop.wait(0.2)
            except (OSError, ValueError):
                pass
            finally:
                with self.printer.lock:
                    self.streams = max(0, self.streams - 1)
                self.printer.event("camera_close", {"streams": self.streams}, transport="http", path="/video", port=CAMERA_PORT)

        return Response(200, None, ctype="multipart/x-mixed-replace; boundary=frame", kind="camera", stream=stream,
                        payload={"streams": self.streams, "limit": self.viewer_limit()})

    # ---- the websocket ----------------------------------------------------------------------
    def _refuse_handshake(self, handler, req, why, fault=None):
        body = json.dumps({"error": why}).encode()
        try:
            handler.send_response(403)
            handler.send_header("Content-Type", "application/json")
            handler.send_header("Content-Length", str(len(body)))
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.write(body)
        except OSError:
            pass
        self.printer.event("ws_refused", {"refused": True, "why": why, "fault": fault, "open": len(self.clients)},
                           path=req.path, port=req.port, status=403)

    def ws_session(self, handler, req):
        with self.printer.lock:
            if req.port != SDCP_PORT or req.path.rstrip("/") != "/websocket":
                return self._refuse_handshake(handler, req, "no websocket on %s:%d" % (req.path, req.port))
            if fault_on(self.faults, "ws_refuse"):
                return self._refuse_handshake(handler, req, "handshake refused", "ws_refuse")
            if fault_on(self.faults, "viewers_full") or len(self.clients) >= self.viewer_limit():
                return self._refuse_handshake(handler, req, "too many clients (limit %d)" % self.viewer_limit(),
                                              "viewers_full" if fault_on(self.faults, "viewers_full") else None)
            if not ws.handshake(handler):
                return self._refuse_handshake(handler, req, "no Sec-WebSocket-Key")
            conn = ws.WsConnection(handler, req.client)
            self.clients.add(conn)
            n = len(self.clients)
        self.printer.event("ws_open", {"open": n, "user_agent": req.headers.get("User-Agent", "")}, path=req.path, port=req.port)
        self._ensure_pusher()
        try:
            while True:
                op, data = conn.recv()
                if op != ws.OP_TEXT:
                    continue
                with self.printer.lock:
                    if fault_on(self.faults, "mute"):
                        replies = []
                        self.printer.event("ws_muted", {"text": data[:200].decode("utf-8", "replace"), "fault": "mute"},
                                           port=req.port)
                    else:
                        replies = self.ws_message(data.decode("utf-8", "replace"), req)
                for r in replies:
                    conn.send_text(r if isinstance(r, str) else json.dumps(r))
                with self.printer.lock:
                    after = self.after_reply
                    self.after_reply = None
                if after:
                    after()
        except (EOFError, OSError, ValueError):
            pass
        finally:
            with self.printer.lock:
                self.clients.discard(conn)
                n = len(self.clients)
            self.printer.event("ws_close", {"open": n}, path=req.path, port=req.port)

    after_reply = None

    def ws_message(self, text, req):
        """One text frame -> the frames to send back (status changes are broadcast separately)."""
        if text.strip() == "ping":
            return ["pong"]
        try:
            msg = json.loads(text)
            d = msg.get("Data") or {}
            cmd, rid, body = d.get("Cmd"), d.get("RequestID", ""), d.get("Data") or {}
        except (ValueError, AttributeError):
            self.printer.event("ws_cmd", {"error": "not JSON", "text": text[:200]}, port=req.port)
            return []
        if cmd == 0:
            self.printer.event("status_query", {"cmd": 0}, port=req.port)
            return [self.status_frame()]
        if cmd == 1:
            self.printer.event("attributes", {"cmd": 1}, port=req.port)
            return [self.attributes_frame()]
        if cmd == 324:
            if not self.canvas_attached():
                self.printer.event("canvas", {"cmd": 324, "ack": 1, "attached": False}, port=req.port)
                return [self.response(324, rid, {"Ack": 1})]
            payload = self.canvas_payload()
            self.printer.event("canvas", {"cmd": 324, "ack": 0, "attached": True, "trays": len(payload["canvas_list"][0]["tray_list"]),
                                          "active_tray_id": payload["active_tray_id"]}, port=req.port)
            return [self.response(324, rid, {"Ack": 0}), self.response(324, rid, payload)]
        if cmd == 128:
            return [self.response(128, rid, {"Ack": self.start_print(body, req)})]
        if cmd in (129, 130, 131):
            ok = (cmd == 129 and self.print_state == "printing") or (cmd == 131 and self.print_state == "paused") or \
                 (cmd == 130 and self.busy())
            self.printer.event("ws_cmd", {"cmd": cmd, "action": {129: "pause", 130: "stop", 131: "resume"}[cmd],
                                          "ack": 0 if ok else 1, "refused": not ok}, port=req.port)
            if ok:
                final = {129: "paused", 131: "printing", 130: "cancelled"}[cmd]

                def finish():
                    with self.printer.lock:
                        self.set_state(final)
                self.after_reply = lambda: threading.Timer(1.0, finish).start()
            return [self.response(cmd, rid, {"Ack": 0 if ok else 1})]
        if cmd == 386:
            enabled = (self.cfg.get("camera") or {}).get("enabled", True)
            data = {"Ack": 0, "VideoUrl": "%s:%d/video" % (self.bind(), CAMERA_PORT)} if enabled else {"Ack": 1}
            self.printer.event("camera_url", {"cmd": 386, "enable": body.get("Enable"), "ack": data["Ack"],
                                              "url": data.get("VideoUrl")}, port=req.port)
            return [self.response(386, rid, data)]
        if cmd == 403:
            if isinstance(body.get("LightStatus"), dict):
                self.light = int(body["LightStatus"].get("SecondLight", self.light))
                if isinstance(body["LightStatus"].get("RgbLight"), list):
                    self.rgb = body["LightStatus"]["RgbLight"]
            fans = body.get("TargetFanSpeed")
            if isinstance(fans, dict):
                for k in ("ModelFan", "AuxiliaryFan", "BoxFan"):
                    if k in fans:
                        self.fans[k] = int(fans[k])
            self.printer.event("ws_cmd", {"cmd": 403, "set": body}, port=req.port)
            self.after_reply = self.push_status
            return [self.response(403, rid, {"Ack": 0})]
        self.printer.event("ws_cmd", {"cmd": cmd, "data": body, "ack": 2, "unknown": True}, port=req.port)
        return [self.response(cmd if cmd is not None else -1, rid, {"Ack": 2})]

    def start_print(self, body, req):
        """Cmd 128. Logs the whole command, the slot_map in its three forms, and what each slot_map
        entry names against the trays and the file's own filaments. -> the Ack."""
        fn = body.get("Filename", "") if isinstance(body, dict) else ""
        name = fn.rsplit("/", 1)[-1]
        present = isinstance(body, dict) and "slot_map" in body
        sm = body.get("slot_map") if present else None
        form = "absent" if not present else ("empty" if sm == [] else ("entries" if isinstance(sm, list) else "malformed"))
        f = self.files.get(name)
        trays = {(0, t["tray_id"]): t for t in self.trays()} if self.canvas_attached() else {}
        mapped, warnings = [], []
        for e in (sm if isinstance(sm, list) else []):
            try:
                t, cid, tid = int(e.get("t")), int(e.get("canvas_id")), int(e.get("tray_id"))
            except (TypeError, ValueError, AttributeError):
                warnings.append("malformed entry %r" % (e,))
                continue
            tray = trays.get((cid, tid))
            ftype = next((x["type"] for x in (f or {}).get("filaments", []) if x["t"] == t), "")
            row = {"t": t, "canvas_id": cid, "tray_id": tid, "tray_type": tray["filament_type"] if tray else None,
                   "tray_loaded": bool(tray and tray["status"] > 0), "file_type": ftype}
            row["type_match"] = bool(tray and ftype and tray["filament_type"].upper() == ftype.upper())
            if not tray:
                warnings.append("T%d -> canvas %d tray %d: no such tray%s" % (t, cid, tid, "" if self.canvas_attached() else " (CANVAS detached)"))
            elif tray["status"] == 0:
                warnings.append("T%d -> tray %d is empty" % (t, tid))
            elif ftype and not row["type_match"]:
                warnings.append("T%d is %s, tray %d holds %s" % (t, ftype, tid, tray["filament_type"]))
            mapped.append(row)
        refuse = int(self.faults.get("refuse_start") or 0) if str(self.faults.get("refuse_start") or "0").isdigit() else 0
        if refuse:
            ack, why = refuse, "fault refuse_start"
        elif self.busy():
            ack, why = 1, "busy (%s)" % self.print_state
        elif not f:
            ack, why = 2, "file not uploaded: " + name
        else:
            ack, why = 0, ""
        payload = {"cmd": 128, "file": name, "filename": fn, "slot_map_form": form, "slot_map": sm,
                   "Calibration_switch": body.get("Calibration_switch"), "PrintPlatformType": body.get("PrintPlatformType"),
                   "Tlp_Switch": body.get("Tlp_Switch"), "StartLayer": body.get("StartLayer"),
                   "ack": ack, "ack_text": ACK_TEXT.get(ack, "?"), "refused": ack != 0, "mapped": mapped, "warnings": warnings,
                   "canvas_attached": self.canvas_attached(), "file_filaments": (f or {}).get("filaments", [])}
        if why:
            payload["why"] = why
        if warnings:
            payload["unverified"] = "what a real CC1 does with these slot_map entries"
        if ack == 0:
            self.current_file = name
            self.progress, self.layer, self.ticks = 0, 0, 0
            self.print_state = "printing"
            self.after_reply = self.push_status
        self.last_start = {k: payload[k] for k in ("file", "slot_map_form", "slot_map", "ack")}
        self.printer.event("start", payload, port=req.port, status=200 if ack == 0 else 400)
        return ack

    def _ensure_pusher(self):
        with self.printer.lock:
            if self.pusher and self.pusher.is_alive():
                return
            self.pusher = threading.Thread(target=self._push_loop, name="emu-sdcp-push-%s" % self.printer.id, daemon=True)
            self.pusher.start()

    def _push_loop(self):
        """A printing CC1 pushes its status on every change; here once a second while printing."""
        stop = self.stopping
        while not stop.wait(1.0):
            with self.printer.lock:
                if not self.clients:
                    self.pusher = None
                    return
                if self.print_state != "printing" or fault_on(self.faults, "mute"):
                    continue
                self.ticks += 1
                if self.ticks % 10 == 0:
                    self.layer = min(150, self.layer + 1)
                    self.progress = min(99, self.progress + 1)
                frame = self.status_frame()
            self.broadcast(frame)

    def on_stop(self):
        # A fresh event for the next start; everything waiting on the old one (camera streams, the
        # status pusher) ends now.
        old, self.stopping = self.stopping, threading.Event()
        old.set()
        self.pusher = None
        for c in list(self.clients):
            c.close()
        self.clients.clear()

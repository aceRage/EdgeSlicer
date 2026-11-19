"""Elegoo Centauri Carbon 2 on the LAN, with or without a CANVAS (the "Combo" is the CC2 sold with one).

What EdgeSlicer speaks to it (src/slic3r/Utils/ElegooCc2.hpp). The shapes come from upstream
OrcaSlicer's ElegooLink.cpp (system/info, the chunked PUT) and Elegoo's elegoo-link library
(Apache-2.0: topics, registration, method numbers and field names). No capture of a real CC2 yet:
everything is marked "unverified" in the log payloads.

Listeners, on the printer's own address (the ports are fixed in EdgeSlicer):

  :<port> (80)  GET /system/info?X-Token=<code>   {"error_code":0,"system_info":{"sn":..}}; a wrong
                                                  code answers HTTP 401 {"error_code":1000}
                PUT /upload    chunks: Content-Range "bytes a-b/total", X-File-Name, X-File-MD5
                               (lower-case hex), X-Token; each answers {"error_code":0}; the whole
                               file is MD5-checked when the last byte is in
  :1883         MQTT 3.1.1, user "elegoo", password = the access code ("123456" by default):
                  elegoo/<sn>/api_register  {"client_id","request_id"}
                    -> elegoo/<sn>/<request_id>/register_response {"client_id","error":"ok"}
                  elegoo/<sn>/<client_id>/api_request {"id","method","params"}
                    -> elegoo/<sn>/<client_id>/api_response {"id","method","result":{"error_code",..}}
                  1001 attributes, 1002 the whole status (machine_status, print_status,
                  extruder, heater_bed, ztemperature_sensor, fans, led),
                  2005 CANVAS (canvas_info; an empty canvas_list without one),
                  2004 CANVAS auto-refill {"auto_refill": bool} (logged; refused without a CANVAS),
                  1020 start print (logged with its full config and slot_map),
                  1021 pause, 1022 stop, 1023 resume (logged; refused when the state does not fit)
                  {"type":"PING"} on api_request -> {"type":"PONG"} on api_response
                elegoo/<sn>/api_status: method 6000 pushes, a delta of the status with an id
                  that counts up by one - on every change, and once a second while printing
                  (progress, layer, temperatures move on)

There is no SDCP (:3030) and no camera here: EdgeSlicer does not show a CC2 camera yet.
"""
import hashlib
import json
import re
import threading
import time

from core import mqtt, safety
from core.http import Response
from .moonraker import fault_on

MQTT_PORT = 1883
DEFAULT_CODE = "123456"
STATES = ("standby", "printing", "paused", "complete", "cancelled")
# print_state -> (machine_status.status, sub_status), elegoo-link's table: 1 idle, 2 printing;
# 2075 printing, 2502 paused, 2504 stopped, 2077 "printing completed" (the finished print still on
# the screen).
MACHINE = {"standby": (1, 0), "printing": (2, 2075), "paused": (2, 2502), "complete": (2, 2077), "cancelled": (2, 2504)}
BUSY = ("printing", "paused")
# 1010 "printer is not in printing state" (elegoo-link's error table).
ERR = {"access": 1000, "invalid": 1001, "invalid_param": 1003, "busy": 1009, "not_printing": 1010, "not_found": 1021,
       "leveling": 1026, "md5": 9008}
TOTAL_LAYERS = 120

FAULTS = [
    {"key": "timeout", "label": "Timeout (accept, never answer)", "type": "bool",
     "help": "Every HTTP request is accepted and never answered."},
    {"key": "refuse_connections", "label": "Connection refused", "type": "bool",
     "help": "HTTP and MQTT closed, as a printer that is switched off."},
    {"key": "http_error", "label": "HTTP error per path", "type": "map", "help": "path=code, e.g. /upload=500"},
    {"key": "upload_md5_mismatch", "label": "Upload MD5 mismatch", "type": "bool",
     "help": "The last chunk answers error 9008 and nothing is stored."},
    {"key": "too_many_clients", "label": "Registration: too many clients", "type": "bool",
     "help": "The MQTT registration answers \"too many clients\"."},
    {"key": "mute", "label": "MQTT mute (answer nothing)", "type": "bool",
     "help": "Connects and registers, then never answers a request."},
    {"key": "refuse_start", "label": "Refuse 1020 with error_code", "type": "number",
     "help": "0 = off; 1009 busy, 1021 file not found, 1026 no bed leveling data (once; a forced retry starts)."},
    {"key": "file_check_s", "label": "Receiving the file after upload (seconds)", "type": "number",
     "help": "machine_status 11 for N s after an upload; EdgeSlicer asks again before 1020."},
    {"key": "push_gaps", "label": "Status pushes skip ids", "type": "bool",
     "help": "Every status push id jumps by two: after five, EdgeSlicer must ask 1002 again."},
    {"key": "no_pushes", "label": "No status pushes", "type": "bool",
     "help": "The printer answers requests but never pushes its status (EdgeSlicer's rows go stale)."},
    {"key": "no_pong", "label": "No PONG", "type": "bool",
     "help": "The heartbeat PING is never answered."},
    {"key": "refuse_auto_refill", "label": "Refuse 2004 (auto-refill)", "type": "bool",
     "help": "2004 answers error_code 1003; EdgeSlicer must not start the print then."},
]

ID_RE = re.compile(r"^elegoo/([^/]+)/([^/]+)/api_request$")


def norm_colour(c):
    c = (c or "").strip().upper()
    if c.startswith("0X"):
        c = c[2:]
    if c and not c.startswith("#"):
        c = "#" + c
    return c[:7] if len(c) >= 7 else ""


class ElegooCC2:
    name = "elegoo_cc2"
    label = "Elegoo Centauri Carbon 2 (MQTT, optional CANVAS)"
    host_type_hint = "Elegoo Link"
    FAULTS = FAULTS

    def __init__(self, printer):
        self.printer = printer
        self.broker = None
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
        return bool(self.ccfg.get("attached", False))

    def code(self):
        return str(self.cfg.get("access_code") or DEFAULT_CODE)

    def serial(self):
        return "EMUCC2" + "".join(c for c in self.printer.id.upper() if c.isalnum())[:10]

    def bind(self):
        return self.cfg.get("bind", "127.0.0.1")

    def reset(self):
        st = self.cfg.get("state", "standby")
        self.print_state = st if st in STATES else "standby"
        self.current_file = "emu_job.gcode" if self.print_state in BUSY else ""
        self.files = {}
        self.chunks = {}       # name -> {total, md5, parts {offset: bytes}}
        self.receiving_until = 0.0
        self.last_start = None
        self.leveling_refused = False
        self.registered = set()
        self.progress, self.layer, self.elapsed = (17, 20, 600) if self.print_state in BUSY else (0, 0, 0)
        self.push_id = 0
        self.pushes = 0
        self.pings = 0
        self.controls = []     # [{method, result}] in order, for the snapshot and the tests
        self.last_status = None
        self.cancel_at = 0.0

    def config_changed(self, old):
        if self.cfg.get("state") != old.get("state") and self.cfg.get("state") in STATES:
            self.set_state(self.cfg["state"])

    def set_state(self, state):
        self.print_state = state if state in STATES else "standby"
        if state in BUSY and not self.current_file:
            self.current_file = "emu_job.gcode"
        if state in BUSY:
            self.progress, self.layer = max(self.progress, 1), max(self.layer, 1)
        if state == "complete":
            self.progress, self.layer = 100, TOTAL_LAYERS
        if state == "standby":
            self.current_file = ""
            self.progress, self.layer, self.elapsed = 0, 0, 0
        self.push_status()

    def listen_ports(self):
        return [int(self.cfg.get("port", 80))]

    def snapshot(self):
        return {"print_state": self.print_state, "current_file": self.current_file, "files": sorted(self.files),
                "canvas_attached": self.canvas_attached(),
                "trays": [{"tray_id": t["tray_id"], "type": t["filament_type"], "colour": t["filament_color"],
                           "brand": t["brand"]} for t in self.trays()] if self.canvas_attached() else [],
                "mqtt_clients": self.broker.clients() if self.broker else [], "registered": sorted(self.registered),
                "last_start": self.last_start, "progress": self.progress, "layer": self.layer,
                "auto_refill": bool(self.ccfg.get("auto_refill", False)), "pushes": self.pushes, "pings": self.pings,
                "controls": self.controls[-10:]}

    def hint(self):
        port = int(self.cfg.get("port", 80))
        return {"host_type": self.host_type_hint,
                "address": self.bind() if port == 80 else "%s:%d" % (self.bind(), port),
                "printer_preset": self.cfg.get("edgeslicer_preset") or "Elegoo Centauri Carbon 2 0.4 nozzle",
                "api_key": "access code %s (leave empty for the default 123456)" % self.code(),
                "mac_alias": safety.mac_alias_command(self.bind())}

    # ---- the CANVAS -------------------------------------------------------------------------
    def slots(self):
        s = [dict(x) if isinstance(x, dict) else {} for x in (self.cfg.get("slots") or [])][:4]
        while len(s) < 4:
            s.append({"loaded": False})
        return s

    def trays(self):
        out = []
        for i, s in enumerate(self.slots()):
            loaded = bool(s.get("loaded")) and bool((s.get("type") or "").strip())
            t = {"tray_id": i, "brand": "", "filament_type": "", "filament_name": "", "filament_code": "",
                 "filament_color": "", "min_nozzle_temp": 0, "max_nozzle_temp": 0, "status": 0}
            if loaded:
                typ = s["type"].strip()
                t.update({"brand": s.get("brand") or "ELEGOO", "filament_type": typ, "filament_name": s.get("name") or typ,
                          "filament_code": s.get("code") or "0x0001", "filament_color": norm_colour(s.get("color")) or "#FFFFFF",
                          "min_nozzle_temp": int(s.get("min_temp") or 190), "max_nozzle_temp": int(s.get("max_temp") or 230),
                          "status": 1})
            out.append(t)
        return out

    def canvas_info(self):
        if not self.canvas_attached():
            return {"active_canvas_id": -1, "active_tray_id": -1, "auto_refill": False, "canvas_list": []}
        active = self.ccfg.get("active_tray", -1)
        return {"active_canvas_id": 0, "active_tray_id": active if isinstance(active, int) else -1,
                "auto_refill": bool(self.ccfg.get("auto_refill", False)),
                "canvas_list": [{"canvas_id": 0, "connected": 1, "tray_list": self.trays()}]}

    def machine_status(self):
        status, sub = MACHINE.get(self.print_state, (1, 0))
        if time.time() < self.receiving_until:
            status, sub = 11, 0
        return {"status": status, "sub_status": sub, "progress": self.progress if self.print_state != "standby" else 0,
                "exception_status": []}

    def full_status(self):
        """The whole 1002 answer (without error_code), elegoo-link's field names."""
        printing = self.print_state == "printing"
        hot = self.print_state in BUSY
        left = max(0, int((100 - self.progress) * 36)) if hot else 0
        return {
            "machine_status": self.machine_status(),
            "print_status": {"filename": self.current_file, "current_layer": self.layer, "total_layer": TOTAL_LAYERS if self.current_file else 0,
                             "print_duration": self.elapsed, "total_duration": self.elapsed + left, "remaining_time_sec": left},
            "extruder": {"temperature": 220.0 if hot else 26.0, "target": 220 if hot else 0},
            "heater_bed": {"temperature": 60.0 if hot else 25.0, "target": 60 if hot else 0},
            "ztemperature_sensor": {"temperature": 31.0 if hot else 24.0, "measured_max_temperature": 33.0, "measured_min_temperature": 22.0},
            "fans": {"fan": {"speed": 100 if printing else 0, "rpm": 6000 if printing else 0},
                     "aux_fan": {"speed": 40 if printing else 0, "rpm": 2400 if printing else 0},
                     "box_fan": {"speed": 20 if hot else 0, "rpm": 900 if hot else 0},
                     "heater_fan": {"speed": 100 if hot else 0, "rpm": 7000 if hot else 0},
                     "controller_fan": {"speed": 50, "rpm": 3000}},
            "led": {"status": 1 if self.cfg.get("light", True) else 0},
        }

    # ---- status pushes ----------------------------------------------------------------------
    @staticmethod
    def _delta(old, new):
        """What changed, as a nested delta (the printer pushes only what changed)."""
        if not isinstance(old, dict) or not isinstance(new, dict):
            return new
        out = {}
        for k, v in new.items():
            if k not in old:
                out[k] = v
            elif isinstance(v, dict) and isinstance(old[k], dict):
                d = ElegooCC2._delta(old[k], v)
                if d:
                    out[k] = d
            elif old[k] != v:
                out[k] = v
        return out

    def push_status(self, force=False):
        """Publish what changed since the last push on elegoo/<sn>/api_status (method 6000)."""
        if not self.broker or fault_on(self.faults, "no_pushes"):
            return
        full = self.full_status()
        delta = full if self.last_status is None else self._delta(self.last_status, full)
        self.last_status = full
        if not delta and not force:
            return
        self.push_id += 2 if fault_on(self.faults, "push_gaps") else 1
        self.pushes += 1
        self._reply("elegoo/%s/api_status" % self.serial(), {"id": self.push_id, "method": 6000, "result": delta})
        self._ensure_pusher()

    def _ensure_pusher(self):
        if self.pusher and self.pusher.is_alive():
            return
        self.pusher = threading.Thread(target=self._push_loop, name="emu-cc2-push-%s" % self.printer.id, daemon=True)
        self.pusher.start()

    def _push_loop(self):
        """A printing CC2 moves on: once a second the progress, the layer and the time, then a push."""
        stop = self.stopping
        tick = 0
        while not stop.wait(1.0):
            with self.printer.lock:
                if not self.broker or not self.broker.clients():
                    self.pusher = None
                    return
                if self.print_state == "cancelled" and self.cancel_at and time.time() >= self.cancel_at:
                    self.cancel_at = 0.0
                    self.print_state, self.current_file = "standby", ""
                    self.progress, self.layer, self.elapsed = 0, 0, 0
                elif self.print_state == "printing":
                    tick += 1
                    self.elapsed += 1
                    if tick % 2 == 0:
                        self.progress = min(99, self.progress + 1)
                        self.layer = min(TOTAL_LAYERS, max(1, self.progress * TOTAL_LAYERS // 100))
                self.push_status()

    # ---- HTTP -------------------------------------------------------------------------------
    def token_ok(self, req):
        return (req.headers.get("X-Token") or req.args.get("X-Token") or "") == self.code()

    def route(self, req):
        p, m = req.path.rstrip("/") or "/", req.method
        if m == "GET" and p == "/system/info":
            if not self.token_ok(req):
                return Response(401, {"error_code": ERR["access"], "message": "token invalid"}, kind="info",
                                payload={"refused": True, "why": "wrong access code"})
            return Response(200, {"error_code": 0, "system_info": {"sn": self.serial(), "hostname": self.cfg.get("name") or "EMU CC2",
                                                                    "machine_model": "Centauri Carbon 2",
                                                                    "firmware_version": "emu"}},
                            kind="info", payload={"sn": self.serial(), "unverified": "field set beyond sn"})
        if m == "PUT" and p == "/upload":
            return self.upload_chunk(req)
        return Response(404, {"error_code": 404, "message": "not found"}, kind="http")

    def upload_chunk(self, req):
        if not self.token_ok(req):
            return Response(401, {"error_code": ERR["access"]}, kind="upload", payload={"refused": True, "why": "wrong access code"})
        name = req.headers.get("X-File-Name") or ""
        md5 = (req.headers.get("X-File-MD5") or "").lower()
        rng = re.match(r"bytes (\d+)-(\d+)/(\d+)$", req.headers.get("Content-Range") or "")
        if not name or not rng:
            return Response(400, {"error_code": ERR["invalid"]}, kind="upload",
                            payload={"refused": True, "why": "missing X-File-Name or Content-Range"})
        first, last, total = int(rng.group(1)), int(rng.group(2)), int(rng.group(3))
        data = req.body or b""
        chunk = {"name": name, "first": first, "last": last, "total": total, "length": len(data), "md5_claimed": md5,
                 "range_matches_body": last - first + 1 == len(data)}
        job = self.chunks.setdefault(name, {"total": total, "md5": md5, "parts": {}})
        job["parts"][first] = data
        got = sum(len(v) for v in job["parts"].values())
        if got < total:
            return Response(200, {"error_code": 0}, kind="upload_chunk", payload=chunk)
        del self.chunks[name]
        whole = b"".join(job["parts"][o] for o in sorted(job["parts"]))
        actual = hashlib.md5(whole).hexdigest()
        info = {"name": name, "size": len(whole), "md5": actual, "md5_claimed": md5, "chunks": len(job["parts"])}
        if actual != md5 or fault_on(self.faults, "upload_md5_mismatch"):
            info.update({"refused": True, "error": "MD5 check failed", "unverified": "error code 9008"})
            return Response(200, {"error_code": ERR["md5"]}, kind="upload", payload=info)
        stored = self.printer.store.put(self.printer.id, name, whole)
        info["stored_at"] = stored["stored_at"]
        self.files[name] = info
        try:
            check_s = float(self.faults.get("file_check_s") or 0)
        except (TypeError, ValueError):
            check_s = 0
        if check_s > 0:
            self.receiving_until = time.time() + check_s
        self.printer.sub_log(req, "upload_chunk", chunk)
        return Response(200, {"error_code": 0}, kind="upload", payload=info)

    # ---- MQTT -------------------------------------------------------------------------------
    def reconcile_extra(self, host):
        want = self.cfg.get("enabled", True) and not fault_on(self.faults, "refuse_connections")
        if self.broker and (not want or self.broker.host != host):
            self.broker.down()
            self.broker = None
        if want and not self.broker:
            b = mqtt.Broker("cc2", host, MQTT_PORT, on_message=self._message, note=self._note,
                            auth=lambda user, pw: user == "elegoo" and pw == self.code())
            try:
                b.up()
                self.broker = b
            except OSError as e:
                return ["cannot listen on %s:%d for MQTT (%s)" % (host, MQTT_PORT, e.strerror or e)]
        return []

    def on_stop(self):
        old, self.stopping = self.stopping, threading.Event()
        old.set()
        self.pusher = None
        if self.broker:
            self.broker.down()
            self.broker = None

    def _note(self, broker, kind, payload):
        self.printer.event("mqtt_" + kind, dict(payload, broker=broker), transport="mqtt")

    def _reply(self, topic, obj):
        self.broker.route(topic, json.dumps(obj).encode())

    def _message(self, session, topic, payload):
        sn = self.serial()
        try:
            body = json.loads(payload)
        except ValueError:
            body = None
        if topic == "elegoo/%s/api_register" % sn:
            if not isinstance(body, dict):
                return
            cid, rid = body.get("client_id", ""), body.get("request_id", "")
            err = "too many clients" if fault_on(self.faults, "too_many_clients") else "ok"
            if err == "ok":
                self.registered.add(cid)
            self.printer.event("register", {"client_id": cid, "request_id": rid, "error": err}, transport="mqtt", path=topic)
            self._reply("elegoo/%s/%s/register_response" % (sn, rid), {"client_id": cid, "error": err})
            return
        m = ID_RE.match(topic)
        if not m or m.group(1) != sn or not isinstance(body, dict):
            return
        cid = m.group(2)
        if body.get("type") == "PING" and "method" not in body:
            self.pings += 1
            if not fault_on(self.faults, "no_pong") and not fault_on(self.faults, "mute"):
                self._reply("elegoo/%s/%s/api_response" % (sn, cid), {"type": "PONG"})
            return
        if fault_on(self.faults, "mute"):
            self.printer.event("mqtt_muted", {"topic": topic, "fault": "mute"}, transport="mqtt", path=topic)
            return
        with self.printer.lock:
            method = body.get("method")
            before = self.print_state
            result = self.request(method, body.get("params") or {}, cid, topic)
            self._reply("elegoo/%s/%s/api_response" % (sn, cid), {"id": body.get("id"), "method": method, "result": result})
            if self.print_state != before or method == 2004:
                self.push_status()

    def request(self, method, params, cid, topic):
        if cid not in self.registered:
            self.printer.event("request", {"method": method, "refused": True, "why": "not registered"}, transport="mqtt", path=topic)
            return {"error_code": ERR["access"]}
        if method == 1001:
            self.printer.event("attributes", {"method": 1001}, transport="mqtt", path=topic)
            return {"error_code": 0, "hostname": self.cfg.get("name") or "EMU CC2", "machine_model": "Centauri Carbon 2",
                    "sn": self.serial(), "software_version": {"ota_version": "emu"}}
        if method == 1002:
            st = self.full_status()
            self.printer.event("status_query", {"method": 1002, "machine_status": st["machine_status"]}, transport="mqtt", path=topic)
            st["error_code"] = 0
            return st
        if method == 2005:
            info = self.canvas_info()
            self.printer.event("canvas", {"method": 2005, "attached": self.canvas_attached(), "trays": len(self.trays())
                                          if self.canvas_attached() else 0}, transport="mqtt", path=topic)
            return {"error_code": 0, "canvas_info": info}
        if method == 1020:
            return self.start_print(params, topic)
        if method in (1021, 1022, 1023):
            return self.print_control(method, topic)
        if method == 2004:
            return self.set_auto_refill(params, topic)
        self.printer.event("request", {"method": method, "params": params, "unknown": True}, transport="mqtt", path=topic)
        return {"error_code": ERR["invalid"]}

    def print_control(self, method, topic):
        """1021 pause, 1022 stop, 1023 resume. Error codes beyond 1010 "not printing" are unverified."""
        name = {1021: "pause", 1022: "stop", 1023: "resume"}[method]
        ok_from = {1021: ("printing",), 1022: ("printing", "paused"), 1023: ("paused",)}[method]
        code = 0 if self.print_state in ok_from else ERR["not_printing"]
        if code == 0:
            if method == 1021:
                self.print_state = "paused"
            elif method == 1023:
                self.print_state = "printing"
            else:
                self.print_state = "cancelled"
                self.cancel_at = time.time() + 3.0
        self.controls.append({"method": method, "action": name, "error_code": code})
        self.printer.event("control", {"method": method, "action": name, "error_code": code, "state": self.print_state,
                                       "unverified": "1023 resume is commented out in elegoo-link's LAN table"},
                           transport="mqtt", path=topic, status=200 if code == 0 else 400)
        return {"error_code": code}

    def set_auto_refill(self, params, topic):
        want = params.get("auto_refill")
        code = 0
        if not isinstance(want, bool):
            code = ERR["invalid_param"]
        elif not self.canvas_attached() or fault_on(self.faults, "refuse_auto_refill"):
            code = ERR["invalid_param"]
        if code == 0:
            canvas = dict(self.ccfg)
            canvas["auto_refill"] = want
            self.cfg["canvas"] = canvas
        self.printer.event("auto_refill", {"method": 2004, "auto_refill": want, "error_code": code,
                                           "unverified": "the error code without a CANVAS"},
                           transport="mqtt", path=topic, status=200 if code == 0 else 400)
        return {"error_code": code}

    def start_print(self, params, topic):
        cfg = params.get("config") or {}
        name = params.get("filename", "")
        slot_map = cfg.get("slot_map")
        payload = {"method": 1020, "filename": name, "storage_media": params.get("storage_media"),
                   "delay_video": cfg.get("delay_video"), "printer_check": cfg.get("printer_check"),
                   "print_layout": cfg.get("print_layout"), "bedlevel_force": cfg.get("bedlevel_force"),
                   "slot_map": slot_map, "unverified": "error codes and the refusal of a bad slot_map"}
        refuse = 0
        try:
            refuse = int(self.faults.get("refuse_start") or 0)
        except (TypeError, ValueError):
            refuse = 0
        code = 0
        if self.print_state in BUSY:
            code = ERR["busy"]
        elif name not in self.files:
            code = ERR["not_found"]
        elif refuse == ERR["leveling"]:
            code = 0 if cfg.get("bedlevel_force") else ERR["leveling"]
        elif refuse:
            code = refuse
        trays = {t["tray_id"]: t for t in self.trays()} if self.canvas_attached() else {}
        problems = []
        for e in slot_map or []:
            t = trays.get(e.get("tray_id")) if isinstance(e, dict) else None
            if not isinstance(e, dict) or e.get("canvas_id") != 0 or t is None:
                problems.append({"entry": e, "why": "no such tray"})
            elif not t["filament_type"]:
                problems.append({"entry": e, "why": "empty tray"})
        payload["slot_map_problems"] = problems
        if code == 0 and problems:
            code = ERR["invalid"]
        payload["error_code"] = code
        if code == 0:
            self.current_file, self.print_state = name, "printing"
            self.progress, self.layer, self.elapsed = 0, 0, 0
            self.last_start = payload
        self.printer.event("start", payload, transport="mqtt", path=topic, status=200 if code == 0 else 400)
        return {"error_code": code}

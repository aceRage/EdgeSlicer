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
                  1001 attributes, 1002 status (machine_status.status / sub_status),
                  2005 CANVAS (canvas_info; an empty canvas_list without one),
                  1020 start print (logged with its full config and slot_map)

There is no SDCP (:3030) and no camera here: EdgeSlicer does not show a CC2 camera yet.
"""
import hashlib
import json
import re
import time

from core import mqtt, safety
from core.http import Response
from .moonraker import fault_on

MQTT_PORT = 1883
DEFAULT_CODE = "123456"
STATES = ("standby", "printing", "paused", "complete")
# print_state -> (machine_status.status, sub_status), elegoo-link's table: 1 idle, 2 printing;
# 2077 is "printing completed" (the finished print still on the screen).
MACHINE = {"standby": (1, 0), "printing": (2, 0), "paused": (2, 2502), "complete": (2, 2077)}
BUSY = ("printing", "paused")
ERR = {"access": 1000, "invalid": 1001, "busy": 1009, "not_found": 1021, "leveling": 1026, "md5": 9008}

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

    def config_changed(self, old):
        if self.cfg.get("state") != old.get("state") and self.cfg.get("state") in STATES:
            self.set_state(self.cfg["state"])

    def set_state(self, state):
        self.print_state = state if state in STATES else "standby"
        if state in BUSY and not self.current_file:
            self.current_file = "emu_job.gcode"
        if state == "standby":
            self.current_file = ""

    def listen_ports(self):
        return [int(self.cfg.get("port", 80))]

    def snapshot(self):
        return {"print_state": self.print_state, "current_file": self.current_file, "files": sorted(self.files),
                "canvas_attached": self.canvas_attached(),
                "trays": [{"tray_id": t["tray_id"], "type": t["filament_type"], "colour": t["filament_color"],
                           "brand": t["brand"]} for t in self.trays()] if self.canvas_attached() else [],
                "mqtt_clients": self.broker.clients() if self.broker else [], "registered": sorted(self.registered),
                "last_start": self.last_start}

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
        return {"status": status, "sub_status": sub, "progress": 0}

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
        if fault_on(self.faults, "mute"):
            self.printer.event("mqtt_muted", {"topic": topic, "fault": "mute"}, transport="mqtt", path=topic)
            return
        with self.printer.lock:
            method = body.get("method")
            result = self.request(method, body.get("params") or {}, cid, topic)
        self._reply("elegoo/%s/%s/api_response" % (sn, cid), {"id": body.get("id"), "method": method, "result": result})

    def request(self, method, params, cid, topic):
        if cid not in self.registered:
            self.printer.event("request", {"method": method, "refused": True, "why": "not registered"}, transport="mqtt", path=topic)
            return {"error_code": ERR["access"]}
        if method == 1001:
            self.printer.event("attributes", {"method": 1001}, transport="mqtt", path=topic)
            return {"error_code": 0, "hostname": self.cfg.get("name") or "EMU CC2", "machine_model": "Centauri Carbon 2",
                    "sn": self.serial(), "software_version": {"ota_version": "emu"}}
        if method == 1002:
            ms = self.machine_status()
            self.printer.event("status_query", {"method": 1002, "machine_status": ms}, transport="mqtt", path=topic)
            return {"error_code": 0, "machine_status": ms, "print_status": {"filename": self.current_file}}
        if method == 2005:
            info = self.canvas_info()
            self.printer.event("canvas", {"method": 2005, "attached": self.canvas_attached(), "trays": len(self.trays())
                                          if self.canvas_attached() else 0}, transport="mqtt", path=topic)
            return {"error_code": 0, "canvas_info": info}
        if method == 1020:
            return self.start_print(params, topic)
        self.printer.event("request", {"method": method, "params": params, "unknown": True}, transport="mqtt", path=topic)
        return {"error_code": ERR["invalid"]}

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
            self.last_start = payload
        self.printer.event("start", payload, transport="mqtt", path=topic, status=200 if code == 0 else 400)
        return {"error_code": code}

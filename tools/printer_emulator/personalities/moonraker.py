"""Generic Moonraker / Klipper (design P1).

Answers what EdgeSlicer's Moonraker-speaking hosts ask: OctoPrint compatibility (/api/version,
/api/files/local - what the "Octo/Klipper" host type uses), Moonraker's own API (/server/info,
/printer/info, /printer/objects/query, /server/files/upload, /printer/print/start,
/printer/gcode/script, /machine/timelapse/settings ...).

Routing, the upload handling and the failure modes are lifted from tests/slic3rutils/
fake_moonraker.py and tests/phone/mock_u1_controls.py; the command interpreter is in klipper.py.
"""
import time

from core.http import Response, moonraker_error
from core import multipart
from . import klipper

MOONRAKER_VERSION = "v0.9.3-emu"
BUSY_STATES = ("printing", "paused")
STATES = ("standby", "printing", "paused", "complete", "cancelled", "error")

# One row per fault: key, label, type, help. The control page draws its toggles from this.
FAULTS = [
    {"key": "timeout", "label": "Timeout (accept, never answer)", "type": "bool",
     "help": "Every request is accepted and never answered; the slicer's own timeout decides."},
    {"key": "refuse_connections", "label": "Connection refused", "type": "bool",
     "help": "The listener is closed, as a printer that is switched off."},
    {"key": "probe_delay_s", "label": "Slow probe (seconds)", "type": "number",
     "help": "Delays /printer/objects/query. Over 10 s outlasts the QIDI Box probe timeout."},
    {"key": "http_error", "label": "HTTP error per path", "type": "map",
     "help": "path=code, one per line, e.g. /server/files/upload=500"},
    {"key": "refuse_script", "label": "Refuse every G-code script", "type": "bool",
     "help": "/printer/gcode/script answers a Klipper error and runs nothing."},
    {"key": "upload_fail", "label": "Upload fails (HTTP 500)", "type": "bool", "help": ""},
    {"key": "upload_8080_only", "label": "Upload only on :8080 (Moonraker retry)", "type": "bool",
     "help": "Uploads on the main port answer 500; a second listener on <bind>:8080 accepts them."},
    {"key": "refuse_start", "label": "Refuse print start", "type": "bool", "help": ""},
    {"key": "timelapse_404", "label": "Timelapse settings 404", "type": "bool", "help": ""},
]


def fault_on(faults, key):
    v = (faults or {}).get(key)
    return bool(v) and v not in ("0", "false", "False")


class Moonraker:
    name = "moonraker"
    label = "Moonraker / Klipper (generic)"
    host_type_hint = "Octo/Klipper"
    FAULTS = FAULTS

    def __init__(self, printer):
        self.printer = printer
        self.reset()

    # ---- config and state -------------------------------------------------------------------
    @property
    def cfg(self):
        return self.printer.cfg

    @property
    def faults(self):
        return self.cfg.get("faults") or {}

    def reset(self):
        self.print_state = self.cfg.get("state", "standby") if self.cfg.get("state", "standby") in STATES else "standby"
        self.current_file = ""
        self.files = {}          # name -> {name, size, sha256, ...}
        self.started = []
        self.timelapse = {"enabled": False}
        self.print_started_at = 0.0

    def config_changed(self, old):
        if self.cfg.get("state") != old.get("state") and self.cfg.get("state") in STATES:
            self.set_state(self.cfg["state"])

    def set_state(self, state):
        self.print_state = state
        if state in BUSY_STATES and not self.current_file:
            self.current_file = "emu_job.gcode"
        if state == "printing":
            self.print_started_at = time.time()

    def busy(self):
        return self.print_state in BUSY_STATES

    def listen_ports(self):
        ports = [int(self.cfg.get("port", 7125))]
        if fault_on(self.faults, "upload_8080_only") and 8080 not in ports:
            ports.append(8080)
        return ports

    def hostname(self):
        return "emu-" + self.printer.id

    def snapshot(self):
        return {"print_state": self.print_state, "current_file": self.current_file,
                "files": sorted(self.files), "timelapse": dict(self.timelapse)}

    def hint(self):
        return {"host_type": self.host_type_hint, "address": "%s:%s" % (self.cfg.get("bind"), self.cfg.get("port")),
                "printer_preset": self.cfg.get("edgeslicer_preset", "any Klipper printer preset")}

    # ---- objects ----------------------------------------------------------------------------
    def objects(self):
        """Every Klipper object this printer has, full. A query picks from these."""
        printing = self.busy()
        elapsed = max(0.0, time.time() - self.print_started_at) if printing else 0.0
        objs = {
            "webhooks": {"state": "ready", "state_message": "Printer is ready"},
            "print_stats": {"state": self.print_state, "filename": self.current_file if printing else "",
                            "message": "", "print_duration": elapsed, "total_duration": elapsed,
                            "filament_used": 0.0, "info": {"current_layer": 3 if printing else 0,
                                                           "total_layer": 50 if printing else 0}},
            "virtual_sdcard": {"file_path": ("/home/emu/printer_data/gcodes/" + self.current_file) if printing else None,
                               "progress": 0.1 if printing else 0.0, "is_active": self.print_state == "printing",
                               "file_position": 0, "file_size": 0},
            "display_status": {"progress": 0.1 if printing else 0.0, "message": None},
            "heater_bed": {"temperature": 60.0 if printing else 24.0, "target": 60.0 if printing else 0.0, "power": 0.0},
            "extruder": {"temperature": 220.0 if printing else 25.0, "target": 220.0 if printing else 0.0,
                         "power": 0.0, "nozzle_diameter": 0.4, "can_extrude": printing},
            "toolhead": {"homed_axes": "xyz" if printing else "", "position": [0, 0, 0, 0], "extruder": "extruder"},
            "idle_timeout": {"state": "Printing" if printing else "Idle"},
        }
        return objs

    def query_objects(self, spec):
        """spec: [(object name, [attrs] or None)] -> Moonraker's result.status. Objects the printer
        does not have are left out, as a real one does."""
        objs = self.objects()
        status = {}
        for name, attrs in spec:
            if name not in objs:
                continue
            o = objs[name]
            status[name] = {a: o[a] for a in attrs if a in o} if attrs else o
        return {"result": {"eventtime": time.time(), "status": status}}

    @staticmethod
    def parse_query(raw_query):
        """`print_stats&output_pin%20polar_cooler&save_variables=variables` -> [(name, attrs)]."""
        from urllib.parse import unquote_plus
        out = []
        for part in (raw_query or "").split("&"):
            if not part:
                continue
            name, _, attrs = part.partition("=")
            name = unquote_plus(name)
            attrs = [a for a in unquote_plus(attrs).split(",") if a] if attrs else None
            out.append((name, attrs))
        return out

    def query_kind(self, names):
        return "query"

    # ---- routing ----------------------------------------------------------------------------
    def route(self, req):
        p, m = req.path.rstrip("/") or "/", req.method
        if m in ("GET", "HEAD"):
            if p == "/server/info":
                return Response(200, {"result": {"klippy_connected": True, "klippy_state": "ready",
                                                 "components": ["file_manager", "octoprint_compat", "timelapse",
                                                                "machine", "history"],
                                                 "failed_components": [], "registered_directories": ["gcodes", "config"],
                                                 "warnings": [], "websocket_count": 0,
                                                 "moonraker_version": MOONRAKER_VERSION, "api_version": [1, 5, 0],
                                                 "api_version_string": "1.5.0"}}, kind="info")
            if p == "/printer/info":
                return Response(200, {"result": {"state": "ready", "state_message": "Printer is ready",
                                                 "hostname": self.hostname(), "software_version": "v0.12.0-emu",
                                                 "cpu_info": "emulator", "klipper_path": "/home/emu/klipper",
                                                 "python_path": "/home/emu/klippy-env/bin/python",
                                                 "log_file": "/home/emu/printer_data/logs/klippy.log",
                                                 "config_file": "/home/emu/printer_data/config/printer.cfg"}},
                                kind="info")
            if p == "/api/version":
                return Response(200, {"server": "1.5.0", "api": "0.1", "text": "OctoPrint (Moonraker %s)" % MOONRAKER_VERSION},
                                kind="info")
            if p == "/api/server":
                return Response(200, {"server": "1.5.0", "safemode": None}, kind="info")
            if p == "/access/info":
                return Response(200, {"result": {"default_source": "moonraker", "available_sources": ["moonraker"],
                                                 "login_required": False, "trusted": True}}, kind="info")
            if p == "/printer/objects/list":
                return Response(200, {"result": {"objects": sorted(self.objects())}}, kind="query")
            if p == "/printer/objects/query":
                return self.handle_query(req, self.parse_query(req.query))
            if p == "/printer/gcode/help":
                return Response(200, {"result": self.gcode_help()}, kind="info")
            if p == "/server/files/list":
                return Response(200, {"result": [{"path": n, "size": f["size"], "modified": f["stored_at"],
                                                  "permissions": "rw"} for n, f in self.files.items()]}, kind="files")
            if p == "/server/files/metadata":
                name = req.args.get("filename", "")
                f = self.files.get(name)
                if not f:
                    return moonraker_error(404, "Metadata not available for " + name, kind="files")
                return Response(200, {"result": {"filename": name, "size": f["size"], "modified": f["stored_at"],
                                                 "slicer": "EdgeSlicer"}}, kind="files")
            if p.startswith("/server/files/config/"):
                return self.config_file(p[len("/server/files/config/"):])
            if p == "/machine/timelapse/settings":
                return self.timelapse_settings(req)
            if p == "/api/job":
                return Response(200, {"job": {"file": {"name": self.current_file or None}},
                                      "progress": {"completion": 10.0 if self.busy() else None},
                                      "state": {"printing": "Printing", "paused": "Paused"}.get(self.print_state,
                                                                                               "Operational")},
                                kind="query")
            if p == "/api/printer":
                return Response(200, {"state": {"text": self.print_state,
                                                "flags": {"printing": self.print_state == "printing",
                                                          "paused": self.print_state == "paused",
                                                          "operational": True, "ready": not self.busy()}},
                                      "temperature": {"bed": {"actual": 24.0, "target": 0.0},
                                                      "tool0": {"actual": 25.0, "target": 0.0}}}, kind="query")
            if p == "/":
                return Response(200, "<html><body>Moonraker emulator %s (%s)</body></html>" % (MOONRAKER_VERSION, self.printer.id),
                                ctype="text/html", kind="info")
        if m == "POST":
            if p == "/printer/objects/query":
                body = req.json_body().get("objects") or {}
                return self.handle_query(req, [(k, v or None) for k, v in body.items()])
            if p == "/printer/gcode/script":
                script = req.args.get("script") or req.json_body().get("script") or ""
                return self.handle_script(req, script)
            if p == "/server/files/upload":
                return self.handle_upload(req, octoprint=False)
            if p == "/api/files/local":
                return self.handle_upload(req, octoprint=True)
            if p == "/printer/print/start":
                return self.start_print(req.args.get("filename") or req.json_body().get("filename") or "", via="print/start")
            if p in ("/printer/print/pause", "/printer/print/resume", "/printer/print/cancel"):
                return self.print_control(p.rsplit("/", 1)[-1])
            if p == "/machine/timelapse/settings":
                return self.timelapse_settings(req)
            if p in ("/printer/emergency_stop", "/printer/firmware_restart", "/printer/restart"):
                self.print_state = "standby" if p != "/printer/emergency_stop" else "error"
                return Response(200, {"result": "ok"}, kind="control", payload={"action": p.rsplit("/", 1)[-1]})
        if m == "DELETE" and p.startswith("/server/files/gcodes/"):
            name = p[len("/server/files/gcodes/"):]
            if name not in self.files:
                return moonraker_error(404, "File does not exist: " + name, kind="files")
            del self.files[name]
            return Response(200, {"result": {"item": {"path": name, "root": "gcodes"}, "action": "delete_file"}},
                            kind="files", payload={"deleted": name})
        return moonraker_error(404, "Not Found", kind="http")

    # ---- handlers ---------------------------------------------------------------------------
    def handle_query(self, req, spec):
        delay = float(self.faults.get("probe_delay_s") or 0)
        if delay > 0:
            self.printer.sleep_interruptible(delay)
        names = [n for n, _a in spec]
        resp = Response(200, self.query_objects(spec), kind=self.query_kind(names),
                        payload={"objects": names, "delayed_s": delay} if delay else {"objects": names})
        return resp

    def gcode_help(self):
        return {"SAVE_VARIABLE": "Save arbitrary variables to disk", "PAUSE": "Pauses the current print",
                "RESUME": "Resumes the print from a pause", "CANCEL_PRINT": "Cancel the current print"}

    def handle_script(self, req, script):
        if fault_on(self.faults, "refuse_script"):
            return moonraker_error(400, "Script refused (emulator fault refuse_script)", kind="script",
                                   payload={"script": script, "refused": True, "fault": "refuse_script"})
        lines = klipper.split_script(script)
        ran = []
        for line in lines:
            try:
                parsed = klipper.parse_line(line)
                if parsed is None:
                    continue
                cmd, params = parsed
                self.run_command(req, cmd, params, line)
                ran.append(cmd)
            except klipper.GcodeError as e:
                return moonraker_error(400, str(e), kind="script",
                                       payload={"script": script, "ran": ran, "failed_line": line, "error": str(e)})
        return Response(200, {"result": "ok"}, kind="script", payload={"script": script, "ran": ran})

    def run_command(self, req, cmd, params, line):
        """One G-code command. Subclasses add theirs; unknown commands are what Klipper does with
        them: an 'Unknown command' note, not an error."""
        if cmd in ("PAUSE", "RESUME", "CANCEL_PRINT"):
            self.print_control({"PAUSE": "pause", "RESUME": "resume", "CANCEL_PRINT": "cancel"}[cmd], log=False)
            self.printer.sub_log(req, "gcode", {"cmd": cmd, "line": line})
            return
        known = cmd in ("G28", "G1", "G0", "G90", "G91", "G92", "M104", "M109", "M140", "M190", "M106", "M107",
                        "M117", "M118", "M220", "M221", "M400", "M84", "SET_LED", "SET_HEATER_TEMPERATURE",
                        "SET_FAN_SPEED", "BED_MESH_CALIBRATE", "BED_MESH_PROFILE")
        self.printer.sub_log(req, "gcode", {"cmd": cmd, "params": params, "line": line,
                                            "result": "ok" if known else 'Unknown command:"%s"' % cmd})

    def handle_upload(self, req, octoprint):
        fields, files = multipart.parse(req.headers.get("Content-Type", ""), req.body)
        if not files:
            return moonraker_error(400, "no file in the upload", kind="upload", payload={"fields": fields})
        _field, filename, data = files[0]
        sub = (fields.get("path") or "").strip("/")
        name = (sub + "/" if sub else "") + filename
        want_print = (fields.get("print") or "").lower() == "true"
        if fault_on(self.faults, "upload_fail"):
            return moonraker_error(500, "Upload failed (emulator fault upload_fail)", kind="upload",
                                   payload={"name": name, "refused": True, "fault": "upload_fail"})
        if fault_on(self.faults, "upload_8080_only") and req.port != 8080:
            return moonraker_error(500, "Upload refused on this port (emulator fault upload_8080_only); retry on 8080",
                                   kind="upload", payload={"name": name, "refused": True, "fault": "upload_8080_only"})
        info = self.printer.store.put(self.printer.id, name, data)
        info["print"] = want_print
        info["root"] = fields.get("root", "gcodes")
        info["api"] = "octoprint" if octoprint else "moonraker"
        self.files[name] = info
        payload = {"name": name, "size": info["size"], "sha256": info["sha256"], "print": want_print,
                   "api": info["api"], "port": req.port}
        started = False
        if want_print:
            # The upload is logged first, then the start it carried, so expect_order reads
            # upload -> start exactly as the printer saw them.
            self.printer.sub_log(req, "upload", payload, status=201)
            r = self.start_print(name, via="upload print=true", req=req)
            started = r.status == 200
        if octoprint:
            body = {"done": True, "files": {"local": {"name": filename, "origin": "local",
                                                      "refs": {"resource": "/api/files/local/" + name}}}}
        else:
            body = {"result": {"item": {"path": name, "root": info["root"]}, "print_started": started,
                               "print_queued": False, "action": "create_file"}}
        return Response(201, body, kind="upload", payload=payload, log=not want_print)

    def start_print(self, name, via, req=None):
        f = self.files.get(name)
        payload = {"file": name, "via": via}
        if f:
            payload.update({"size": f["size"], "sha256": f["sha256"]})
        if fault_on(self.faults, "refuse_start"):
            payload.update({"refused": True, "fault": "refuse_start"})
            r = moonraker_error(400, "Print start refused (emulator fault refuse_start)", kind="start", payload=payload)
        elif not f:
            payload["refused"] = True
            r = moonraker_error(404, "File not found: " + name, kind="start", payload=payload)
        elif self.busy():
            payload.update({"refused": True, "why": "busy (%s)" % self.print_state})
            r = moonraker_error(400, "Printer is busy", kind="start", payload=payload)
        else:
            self.current_file = name
            self.set_state("printing")
            self.started.append(name)
            r = Response(200, {"result": "ok"}, kind="start", payload=payload)
        if req is not None:
            # A start carried by an upload: logged as its own entry, the upload answers for itself.
            self.printer.sub_log(req, "start", r.payload, status=r.status)
        return r

    def print_control(self, action, log=True):
        before = self.print_state
        if action == "pause" and self.print_state == "printing":
            self.print_state = "paused"
        elif action == "resume" and self.print_state == "paused":
            self.print_state = "printing"
        elif action == "cancel" and self.busy():
            self.print_state = "cancelled"
        return Response(200, {"result": "ok"}, kind="control",
                        payload={"action": action, "before": before, "after": self.print_state}, log=log)

    def timelapse_settings(self, req):
        if fault_on(self.faults, "timelapse_404"):
            return moonraker_error(404, "Not Found", kind="timelapse",
                                   payload={"refused": True, "fault": "timelapse_404", "query": req.query})
        if req.method == "POST":
            for k, v in list(req.args.items()) + list(req.json_body().items()):
                if isinstance(v, str):
                    v = {"true": True, "false": False}.get(v.lower(), v)
                self.timelapse[k] = v
        return Response(200, {"result": dict(self.timelapse)}, kind="timelapse",
                        payload={"settings": dict(self.timelapse), "set": dict(req.args)} if req.method == "POST"
                        else {"read": True})

    def config_file(self, name):
        return moonraker_error(404, "File not found: config/" + name, kind="files", payload={"file": name})

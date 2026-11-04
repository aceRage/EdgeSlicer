"""A stand-in print host for the phone send gates: speaks enough OctoPrint (GET /api/version, POST
/api/files/local) and Moonraker (GET /server/info, POST /server/files/upload) for the slicer's PrintHost
classes, and enough of a Snapmaker U1 over the LAN (access/printer/machine info, printer object
queries with four loaded toolheads, file metadata, the toolhead mapping macros and the print start)
for the phone's LAN path. Every upload is stored under <dir>/uploads with a record of the form
fields (notably `print`); the scripts and starts it received are recorded too, readable together at
GET /mock/state. Nothing is ever printed: a "start" only flips this mock's own reported state.

usage: mock_printhost.py <port> <dir>      (runs until killed)
"""
import email, email.policy, json, os, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18089
DIR = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_host")
os.makedirs(os.path.join(DIR, "uploads"), exist_ok=True)
STATE = {"uploads": [], "requests": [], "scripts": [], "starts": [], "files": {}}
# What a U1 answers, copied from a real one (10.0.0.108, firmware 1.5.2). Four toolheads, all
# loaded: the colours below are what the mapping is matched against.
TOOLHEADS = {
    "filament_vendor": ["Snapmaker"] * 4,
    "filament_type": ["PLA", "PLA", "PETG", "PLA"],
    "filament_sub_type": ["Matte", "Matte", "Basic", "Matte"],
    "filament_color": [4294895013, 4292927712, 4294230066, 4278190080],
    "filament_color_rgba": ["FEE5A5FF", "E0E0E0FF", "F4C032FF", "000000FF"],
    "filament_official": [False] * 4,
    "filament_exist": [True, True, True, True],
    "extruder_map_table": [0, 1, 2, 3] + [0] * 28,
    "extruders_used": [False] * 4,
    # What the firmware's SM_PRINT_END_AUTO_UNLOAD_FILAMENT reads at the end of a job. A real U1
    # sets these from SET_PRINT_USED_EXTRUDERS / SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT before
    # the print starts; apply_print_task_script() below does the same to what this mock reports, so
    # a gate can assert what the printer would be holding, not only what was sent to it.
    "filament_soft": [False, False, False, False],
    "end_unload_filament": [False] * 4,
}
# The toolhead-count of the print_task_config arrays on a U1 (PHYSICAL_EXTRUDER_NUM).
TOOLHEAD_COUNT = 4


def apply_print_task_script(script):
    """Mirror the print_task_config setters a U1 accepts before a print start.

    Understood, exactly as klippy/extras/print_task_config.py parses them:
      SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2            -> extruders_used
      SET_PRINT_PREFERENCES ... END_UNLOAD_FILAMENT=[1,0,1,0] -> end_unload_filament
    Anything else is recorded but changes nothing, as on the printer.
    """
    import ast
    for line in script.splitlines():
        line = line.strip()
        if line.startswith("SET_PRINT_USED_EXTRUDERS"):
            for tok in line.split()[1:]:
                if tok.startswith("EXTRUDERS="):
                    used = [False] * TOOLHEAD_COUNT
                    for v in tok[len("EXTRUDERS="):].split(","):
                        try:
                            i = int(v)
                        except ValueError:
                            continue
                        if 0 <= i < TOOLHEAD_COUNT:
                            used[i] = True
                    TOOLHEADS["extruders_used"] = used
        elif line.startswith("SET_PRINT_PREFERENCES") or line.startswith("SET_PRINT_TASK_PARAMETERS"):
            for tok in line.split()[1:]:
                if tok.startswith("END_UNLOAD_FILAMENT="):
                    flags = [False] * TOOLHEAD_COUNT
                    try:
                        got = ast.literal_eval(tok[len("END_UNLOAD_FILAMENT="):])
                    except Exception:
                        continue  # the printer answers "Invalid END_UNLOAD_FILAMENT"
                    if not isinstance(got, list):
                        continue
                    for i in range(min(len(got), TOOLHEAD_COUNT)):
                        flags[i] = bool(got[i])
                    TOOLHEADS["end_unload_filament"] = flags
PRINT_STATE = {"state": "complete", "filename": "", "progress": 0.0}
LOCK = threading.Lock()
CRLF = chr(13) + chr(10)


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

    def do_DELETE(self):
        with LOCK:
            STATE["requests"].append({"method": "DELETE", "path": self.path, "t": time.time()})
        if self.path.startswith("/server/files/gcodes/"):
            from urllib.parse import unquote
            name = unquote(self.path.split("/server/files/gcodes/", 1)[1])
            with LOCK:
                STATE["files"].pop(name, None)
            return self._json(200, {"result": {"action": "delete_file", "item": {"path": name, "root": "gcodes"}}})
        return self._json(404, {"error": "no such route"})

    def do_GET(self):
        with LOCK:
            STATE["requests"].append({"method": "GET", "path": self.path, "t": time.time()})
        if self.path.startswith("/api/version"):
            return self._json(200, {"api": "0.1", "server": "1.9.3", "text": "OctoPrint 1.9.3"})
        if self.path.startswith("/server/info"):
            return self._json(200, {"result": {"klippy_connected": True, "klippy_state": "ready", "moonraker_version": "v0.8.0-mock"}})
        if self.path.startswith("/access/info"):
            return self._json(200, {"result": {"default_source": "moonraker", "available_sources": ["moonraker"],
                                               "login_required": False, "trusted": True}})
        if self.path.startswith("/printer/info"):
            return self._json(200, {"result": {"state": "ready", "state_message": "Printer is ready", "hostname": "lava",
                                               "software_version": "1.5.2-mock"}})
        if self.path.startswith("/machine/system_info"):
            return self._json(200, {"result": {"system_info": {"product_info": {
                "machine_type": "Snapmaker U1", "nozzle_diameter": [0.4] * 4,
                "serial_number": "MOCKU1SERIAL0000001", "device_name": "Mock U1",
                "firmware_version": "1.5.2", "software_version": "1.5.2"}}}})
        if self.path.startswith("/printer/objects/query"):
            with LOCK:
                st = {
                    "print_stats": {"filename": PRINT_STATE["filename"], "state": PRINT_STATE["state"],
                                    "print_duration": 0.0, "total_duration": 0.0, "message": "",
                                    "info": {"total_layer": 100, "current_layer": 0}},
                    "display_status": {"progress": PRINT_STATE["progress"], "message": None},
                    "heater_bed": {"temperature": 24.0, "target": 0.0},
                    "extruder": {"temperature": 25.0, "target": 0.0, "nozzle_diameter": 0.4, "extruder_index": 0},
                    "extruder1": {"temperature": 25.0, "target": 0.0, "nozzle_diameter": 0.4, "extruder_index": 1},
                    "extruder2": {"temperature": 25.0, "target": 0.0, "nozzle_diameter": 0.4, "extruder_index": 2},
                    "extruder3": {"temperature": 25.0, "target": 0.0, "nozzle_diameter": 0.4, "extruder_index": 3},
                    "print_task_config": dict(TOOLHEADS),
                }
            asked = self.path.split("?", 1)[1] if "?" in self.path else ""
            wanted = [k.split("=")[0] for k in asked.split("&") if k]
            if wanted:
                st = {k: v for k, v in st.items() if k in wanted}
            return self._json(200, {"result": {"eventtime": time.time(), "status": st}})
        if self.path.startswith("/server/files/metadata"):
            name = ""
            if "filename=" in self.path:
                from urllib.parse import unquote
                name = unquote(self.path.split("filename=", 1)[1].split("&")[0])
            with LOCK:
                size = STATE["files"].get(name)
            if size is None:
                return self._json(404, {"error": {"code": 404, "message": "File %s not found" % name}})
            return self._json(200, {"result": {"size": size, "modified": time.time(), "filename": name,
                                               "slicer": "Snapmaker Orca", "estimated_time": 600}})
        if self.path.startswith("/server/files/list"):
            with LOCK:
                return self._json(200, {"result": [{"path": n, "size": s, "modified": time.time()}
                                                   for n, s in STATE["files"].items()]})
        if self.path.startswith("/mock/state"):
            with LOCK:
                return self._json(200, STATE)
        if self.path.startswith("/mock/reset"):
            with LOCK:
                STATE["uploads"].clear()
                STATE["requests"].clear()
                STATE["scripts"].clear()
                STATE["starts"].clear()
                STATE["files"].clear()
                PRINT_STATE.update({"state": "complete", "filename": "", "progress": 0.0})
                TOOLHEADS["extruders_used"] = [False] * TOOLHEAD_COUNT
                TOOLHEADS["end_unload_filament"] = [False] * TOOLHEAD_COUNT
            return self._json(200, {"ok": True})
        return self._json(404, {"error": "no such route"})

    def do_POST(self):
        with LOCK:
            STATE["requests"].append({"method": "POST", "path": self.path, "t": time.time()})
        # The Snapmaker LAN path: the toolhead mapping macros, then the start.
        if self.path.startswith("/printer/gcode/script"):
            from urllib.parse import unquote
            script = unquote(self.path.split("script=", 1)[1]) if "script=" in self.path else ""
            script = script.replace("+", " ")
            with LOCK:
                STATE["scripts"].append({"script": script, "t": time.time()})
                apply_print_task_script(script)
            return self._json(200, {"result": "ok"})
        if self.path.startswith("/printer/print/start"):
            from urllib.parse import unquote
            name = unquote(self.path.split("filename=", 1)[1].split("&")[0]) if "filename=" in self.path else ""
            with LOCK:
                known = name in STATE["files"]
                if known:
                    STATE["starts"].append({"filename": name, "t": time.time()})
                    PRINT_STATE.update({"state": "printing", "filename": name, "progress": 0.01})
            if not known:
                return self._json(404, {"error": {"code": 404, "message": "File %s not found" % name}})
            return self._json(200, {"result": "ok"})
        if self.path.startswith("/mock/idle"):
            with LOCK:
                PRINT_STATE.update({"state": "complete", "filename": "", "progress": 0.0})
            return self._json(200, {"ok": True})
        if not (self.path.startswith("/api/files/local") or self.path.startswith("/server/files/upload")):
            return self._json(404, {"error": "no such route"})
        ctype = self.headers.get("Content-Type", "")
        if not ctype.startswith("multipart/form-data"):
            return self._json(400, {"error": "multipart expected"})
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        msg = email.message_from_bytes(("Content-Type: " + ctype + CRLF + CRLF).encode() + raw, policy=email.policy.HTTP)
        fields, item = {}, None
        for part in msg.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if name == "file":
                item = part
            elif name:
                fields[name] = part.get_payload(decode=True).decode("utf-8", "replace")
        if item is None or not item.get_filename():
            return self._json(400, {"error": "no file"})
        data = item.get_payload(decode=True)
        safe = os.path.basename(item.get_filename())
        with open(os.path.join(DIR, "uploads", safe), "wb") as f:
            f.write(data)
        rec = {"api": "octoprint" if self.path.startswith("/api/") else "moonraker", "filename": safe, "size": len(data),
               "fields": fields, "print": str(fields.get("print", "")).lower() == "true", "t": time.time()}
        with LOCK:
            STATE["uploads"].append(rec)
            STATE["files"][safe] = len(data)
        if rec["api"] == "octoprint":
            return self._json(201, {"done": True, "files": {"local": {"name": safe, "origin": "local", "path": safe}}})
        return self._json(201, {"result": {"item": {"path": safe, "root": "gcodes"}, "print_started": rec["print"], "action": "create_file"}})


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print("mock print host on 127.0.0.1:%d, uploads in %s" % (PORT, os.path.join(DIR, "uploads")), flush=True)
    srv.serve_forever()

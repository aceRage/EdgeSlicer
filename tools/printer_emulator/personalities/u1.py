"""Snapmaker U1 on the LAN: Moonraker over HTTP on port 80, four toolheads (design 2.3).

From tests/slic3rutils/fake_moonraker.py (the Moonraker core it already shares with the QIDI
personality) and tests/phone/mock_u1_controls.py (print_task_config, the controls objects, the
gcode/help list and the controls macros), plus the mapped start that neither mock interpreted:

  print_task_config  filament_vendor / filament_type / filament_sub_type / filament_color_rgba /
                     filament_official / filament_exist per toolhead (from the configured slots),
                     extruder_map_table (32 entries), extruders_used, and what the last
                     SET_PRINT_PREFERENCES set
  SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<file filament> MAP_EXTRUDER=<toolhead>
  SET_PRINT_USED_EXTRUDERS EXTRUDERS=<toolhead,...>
  SET_PRINT_PREFERENCES BED_LEVEL= FLOW_CALIBRATE= TIME_LAPSE_CAMERA= [END_UNLOAD_FILAMENT=[1,0,..]]
                     refused while printing or paused, as the firmware's print_task_config does
  SDCARD_PRINT_FILE FILENAME=  and POST /printer/print/start?filename= (404 when never uploaded)
  /machine/system_info  machine_type "Snapmaker U1", serial EMU-U1-..., device_name

Optional (u1.mqtt.enabled): the pairing broker (plain MQTT, port 1884, <code>/config/request) and a
LAN broker (JSON-RPC on <sn>/request -> <sn>/response). Lifted from mock_u1_mqtt.py WITHOUT its
cloud broker. The real LAN broker is MQTT over TLS with the printer's certificate; this one is plain,
so only a test that builds its session without TLS can use it (the product's own LAN session
cannot). UNVERIFIED: the exact error text the firmware gives for a refused SET_PRINT_PREFERENCES.
"""
import ast
import json

from core import mqtt, safety
from core.http import Response
from .moonraker import FAULTS as MOONRAKER_FAULTS, Moonraker, fault_on
from . import klipper

TOOLHEADS = 4
U1_FAULTS = MOONRAKER_FAULTS + [
    {"key": "refuse_extruder_map", "label": "Refuse SET_PRINT_EXTRUDER_MAP", "type": "bool",
     "help": "The mapping script fails on its first line: the print must not start."},
    {"key": "login_required", "label": "Login required (/access/info)", "type": "bool",
     "help": "The phone can only reach a U1 that does not ask for a login."},
    {"key": "start_error_state", "label": "Print start ends in error", "type": "bool",
     "help": "print/start answers ok, then print_stats says error (a job the printer threw out)."},
]
MACROS = {"SET_PRINT_EXTRUDER_MAP": "Map a file's extruder onto a toolhead",
          "SET_PRINT_USED_EXTRUDERS": "The toolheads this print uses",
          "SET_PRINT_PREFERENCES": "Bed level, flow calibration, time-lapse, end unload",
          "SDCARD_PRINT_FILE": "Start a file", "INNER_FILAMENT_UNLOAD": "G-Code macro",
          "PARK_EXTRUDER": "G-Code macro", "SM_PRINT_EXTRUDER_PREHEAT": "G-Code macro",
          "SM_PRINT_AUTO_FEED": "G-Code macro", "SET_HEATER_TEMPERATURE": "Sets a heater temperature",
          "M220": "Speed factor", "M106": "Set fan", "SET_LED": "Set an LED", "SET_FAN_SPEED": "Set a generic fan"}


def rgba(c):
    c = (c or "").strip().upper().lstrip("#")
    if len(c) == 6:
        c += "FF"
    return c if len(c) == 8 else "000000FF"


def as_bool(v):
    return str(v).strip().lower() in ("1", "true", "yes", "on")


class U1(Moonraker):
    name = "u1"
    label = "Snapmaker U1 (Moonraker, 4 toolheads)"
    host_type_hint = "Snapmaker U1 by IP (phone app or Printers tab: Add printer; hub POST /api/snapmaker/add?ip=)"
    FAULTS = U1_FAULTS

    def __init__(self, printer):
        self.brokers = {}
        self.authorized = set()
        self.pending = set()
        super().__init__(printer)

    @property
    def ucfg(self):
        return self.cfg.get("u1") or {}

    def listen_ports(self):
        return [int(self.cfg.get("port", 80))]

    def serial(self):
        return "EMU-U1-" + "".join(c for c in self.printer.id.upper() if c.isalnum())[:12]

    def reset(self):
        super().reset()
        self.extruder_map = list(range(TOOLHEADS)) + [0] * 28
        self.used = [False] * TOOLHEADS
        self.preferences = {}
        self.heaters = {"heater_bed": 0.0, "extruder": 0.0, "extruder1": 0.0, "extruder2": 0.0, "extruder3": 0.0}
        self.speed_factor = 1.0
        self.led = 1.0
        self.fan = 0.0
        self.cavity_fan = 0.0
        self.active = 0
        self.exist_override = {}

    def slots(self):
        s = [dict(x) if isinstance(x, dict) else {} for x in (self.cfg.get("slots") or [])][:TOOLHEADS]
        while len(s) < TOOLHEADS:
            s.append({"loaded": False})
        return s

    def exists(self):
        return [self.exist_override.get(i, bool(s.get("loaded"))) for i, s in enumerate(self.slots())]

    def snapshot(self):
        s = super().snapshot()
        s.update({"extruder_map": self.extruder_map[:8], "extruders_used": list(self.used),
                  "preferences": dict(self.preferences),
                  "toolheads": [{"type": x.get("type", ""), "color": x.get("color", ""), "loaded": e}
                                for x, e in zip(self.slots(), self.exists())]})
        if self.brokers:
            s["mqtt"] = {n: b.clients() for n, b in self.brokers.items()}
        return s

    def hint(self):
        port = int(self.cfg.get("port", 80))
        return {"host_type": self.host_type_hint,
                "address": self.cfg.get("bind") if port == 80 else "%s:%d" % (self.cfg.get("bind"), port),
                "printer_preset": self.cfg.get("edgeslicer_preset") or "Snapmaker U1 (0.4 nozzle)",
                "mac_alias": safety.mac_alias_command(self.cfg.get("bind", ""))}

    # ---- objects ----------------------------------------------------------------------------
    def objects(self):
        objs = super().objects()
        sl = self.slots()
        printing = self.busy()
        objs["print_task_config"] = {
            "filament_vendor": [x.get("vendor", "Snapmaker") if x.get("loaded") else "" for x in sl],
            "filament_type": [x.get("type", "") if x.get("loaded") else "" for x in sl],
            "filament_sub_type": [x.get("sub", "") if x.get("loaded") else "" for x in sl],
            "filament_color_rgba": [rgba(x.get("color")) if x.get("loaded") else "00000000" for x in sl],
            "filament_official": [bool(x.get("official")) for x in sl],
            "filament_exist": self.exists(),
            "extruder_map_table": list(self.extruder_map),
            "extruders_used": list(self.used),
        }
        for k, v in self.preferences.items():
            objs["print_task_config"][k] = v
        for i in range(TOOLHEADS):
            name = "extruder" if i == 0 else "extruder%d" % i
            objs[name] = {"temperature": 220.0 if printing and self.used[i] else 25.0, "target": self.heaters[name],
                          "power": 0.0, "nozzle_diameter": float(sl[i].get("nozzle", 0.4) or 0.4), "extruder_index": i,
                          "can_extrude": printing}
        objs["gcode_move"] = {"speed_factor": self.speed_factor, "speed": 1500.0, "extrude_factor": 1.0}
        objs["fan"] = {"speed": self.fan}
        objs["led cavity_led"] = {"color_data": [[0.0, 0.0, 0.0, self.led]]}
        objs["fan_generic cavity_fan"] = {"speed": self.cavity_fan}
        if self.print_state == "error":
            objs["print_stats"]["message"] = "emulated print error"
        return objs

    def query_kind(self, names):
        return "probe" if "print_task_config" in names else "query"

    def route(self, req):
        p = req.path.rstrip("/") or "/"
        if req.method in ("GET", "HEAD"):
            if p == "/access/info":
                lr = fault_on(self.faults, "login_required")
                return Response(200, {"result": {"default_source": "moonraker", "available_sources": ["moonraker"],
                                                 "login_required": lr, "trusted": not lr}}, kind="info")
            if p == "/machine/system_info":
                return Response(200, {"result": {"system_info": {"product_info": {
                    "machine_type": "Snapmaker U1", "nozzle_diameter": [0.4] * TOOLHEADS, "serial_number": self.serial(),
                    "device_name": self.cfg.get("name") or "EMU U1", "firmware_version": "1.5.2-emu",
                    "software_version": "1.5.2-emu"}}}}, kind="info")
        return super().route(req)

    def gcode_help(self):
        h = super().gcode_help()
        h.update(MACROS)
        return h

    # ---- the interpreter --------------------------------------------------------------------
    def _int(self, params, key, lo, hi, cmd):
        try:
            v = int(params[key])
        except KeyError:
            raise klipper.GcodeError("Error on '%s': missing %s" % (cmd, key))
        except ValueError:
            raise klipper.GcodeError("Error on '%s': unable to parse %s" % (cmd, params[key]))
        if not lo <= v <= hi:
            raise klipper.GcodeError("Error on '%s': %s must be between %d and %d" % (cmd, key, lo, hi))
        return v

    def run_command(self, req, cmd, params, line):
        if cmd == "SET_PRINT_EXTRUDER_MAP":
            if fault_on(self.faults, "refuse_extruder_map"):
                self.printer.sub_log(req, "extruder_map", {"line": line.strip(), "refused": True, "fault": "refuse_extruder_map"},
                                     status=400)
                raise klipper.GcodeError("SET_PRINT_EXTRUDER_MAP refused (emulator fault refuse_extruder_map)")
            c = self._int(params, "CONFIG_EXTRUDER", 0, 31, cmd)
            m = self._int(params, "MAP_EXTRUDER", 0, TOOLHEADS - 1, cmd)
            self.extruder_map[c] = m
            self.printer.sub_log(req, "extruder_map", {"config": c, "map": m, "line": line.strip(),
                                                       "toolhead_loaded": self.exists()[m],
                                                       "toolhead_type": self.slots()[m].get("type", "")})
            return
        if cmd == "SET_PRINT_USED_EXTRUDERS":
            raw = params.get("EXTRUDERS", "")
            try:
                heads = [int(x) for x in raw.split(",") if x.strip() != ""]
            except ValueError:
                raise klipper.GcodeError("Error on 'SET_PRINT_USED_EXTRUDERS': unable to parse %s" % raw)
            if any(not 0 <= h < TOOLHEADS for h in heads):
                raise klipper.GcodeError("Error on 'SET_PRINT_USED_EXTRUDERS': toolhead out of range in %s" % raw)
            self.used = [i in heads for i in range(TOOLHEADS)]
            self.printer.sub_log(req, "used_extruders", {"extruders": heads, "line": line.strip()})
            return
        if cmd == "SET_PRINT_PREFERENCES":
            if self.busy():
                self.printer.sub_log(req, "preferences", {"line": line.strip(), "refused": True,
                                                          "why": "print_task_config refuses it while %s" % self.print_state,
                                                          "unverified": "the firmware's exact wording"}, status=400)
                raise klipper.GcodeError("SET_PRINT_PREFERENCES is not allowed while printing")
            prefs = {}
            for k, v in params.items():
                if k == "END_UNLOAD_FILAMENT":
                    try:
                        val = ast.literal_eval(v)
                    except (ValueError, SyntaxError):
                        raise klipper.GcodeError("Error on 'SET_PRINT_PREFERENCES': unable to parse END_UNLOAD_FILAMENT=%s" % v)
                    if not isinstance(val, (list, tuple)):
                        raise klipper.GcodeError("Error on 'SET_PRINT_PREFERENCES': END_UNLOAD_FILAMENT must be a list")
                    prefs["end_unload_filament"] = [int(bool(x)) for x in list(val)[:TOOLHEADS]] + \
                        [0] * max(0, TOOLHEADS - len(val))
                else:
                    prefs[k.lower()] = as_bool(v)
            self.preferences.update(prefs)
            self.printer.sub_log(req, "preferences", dict(prefs, line=line.strip()))
            return
        if cmd == "SDCARD_PRINT_FILE":
            r = self.start_print(params.get("FILENAME", "").strip('"'), via="SDCARD_PRINT_FILE")
            self.printer.sub_log(req, "start", r.payload, status=r.status)
            if r.status != 200:
                raise klipper.GcodeError(r.body["error"]["message"])
            return
        if self._control(cmd, params):
            self.printer.sub_log(req, "gcode", {"cmd": cmd, "params": params, "line": line.strip(), "result": "ok"})
            return
        super().run_command(req, cmd, params, line)

    def _control(self, cmd, params):
        """The phone's controls and filament macros (mock_u1_controls.py). True when handled."""
        try:
            if cmd == "SET_HEATER_TEMPERATURE" and params.get("HEATER") in self.heaters:
                self.heaters[params["HEATER"]] = float(params.get("TARGET", "0"))
            elif cmd == "M220":
                self.speed_factor = float(params.get("S", "100")) / 100.0
            elif cmd == "SET_LED" and params.get("LED") == "cavity_led":
                self.led = float(params.get("WHITE", "0"))
            elif cmd == "M106":
                self.fan = float(params.get("S", "0")) / 255.0
            elif cmd == "SET_FAN_SPEED" and params.get("FAN") == "cavity_fan":
                self.cavity_fan = float(params.get("SPEED", "0"))
            elif len(cmd) == 2 and cmd[0] == "T" and cmd[1].isdigit():
                self.active = int(cmd[1])
            elif cmd == "INNER_FILAMENT_UNLOAD":
                self.exist_override[self.active] = False
            elif cmd == "SM_PRINT_AUTO_FEED":
                self.exist_override[int(params.get("EXTRUDER", "0"))] = True
            else:
                return False
        except ValueError:
            raise klipper.GcodeError("Error on '%s': unable to parse %s" % (cmd, params))
        return True

    def start_print(self, name, via, req=None):
        r = super().start_print(name, via, req)
        if r.status == 200 and fault_on(self.faults, "start_error_state"):
            self.print_state = "error"
            r.payload["fault"] = "start_error_state"
        if r.status == 200:
            r.payload["extruder_map"] = self.extruder_map[:8]
            r.payload["extruders_used"] = list(self.used)
            r.payload["preferences"] = dict(self.preferences)
        return r

    # ---- MQTT (optional) --------------------------------------------------------------------
    def reconcile_extra(self, host):
        """The pairing and LAN brokers, when u1.mqtt.enabled. -> errors."""
        mc = self.ucfg.get("mqtt") or {}
        want = {}
        if mc.get("enabled") and self.cfg.get("enabled", True) and not fault_on(self.faults, "refuse_connections"):
            want = {"pairing": int(mc.get("pairing_port", 1884)), "lan": int(mc.get("lan_port", 8883))}
        errors = []
        for name in list(self.brokers):
            b = self.brokers[name]
            if want.get(name) != b.port or b.host != host:
                b.down()
                del self.brokers[name]
        for name, port in want.items():
            if name in self.brokers:
                continue
            if name == "pairing":
                b = mqtt.Broker("pairing", host, port, on_message=self._pairing_message, note=self._mqtt_note)
            else:
                b = mqtt.Broker("lan", host, port, admit=self._lan_admit, on_message=self._lan_message, note=self._mqtt_note)
            try:
                b.up()
                self.brokers[name] = b
            except OSError as e:
                errors.append("cannot listen on %s:%d for MQTT %s (%s)" % (host, port, name, e.strerror or e))
        return errors

    def on_stop(self):
        for b in list(self.brokers.values()):
            b.down()
        self.brokers.clear()

    def _mqtt_note(self, broker, kind, payload):
        self.printer.event("mqtt_" + kind, dict(payload, broker=broker), transport="mqtt")

    def _lan_admit(self, cid):
        return cid in self.authorized or cid in set((self.ucfg.get("mqtt") or {}).get("authorized") or [])

    def _pairing_message(self, session, topic, payload):
        mc = self.ucfg.get("mqtt") or {}
        code = str(mc.get("code", "12345678"))
        if not topic.endswith("/config/request"):
            return
        if topic != code + "/config/request":
            self.printer.event("mqtt_pairing", {"topic": topic, "refused": True, "why": "wrong access code"}, transport="mqtt")
            return
        try:
            req = json.loads(payload)
        except ValueError:
            return
        method = req.get("method", "")
        params = req.get("params") or {}
        cid = params.get("clientid", "")
        with self.printer.lock:
            if method.endswith("confirm_lan_status"):
                if cid in self.authorized or cid in set(mc.get("authorized") or []):
                    result = {"state": "success", "clientid": cid, "ca": "EMU-CA", "cert": "EMU-CERT", "key": "EMU-KEY",
                              "port": int(mc.get("lan_port", 8883)), "sn": self.serial()}
                else:
                    result = {"state": "unauthorized", "clientid": cid}
            elif method.endswith("request_lan_auth"):
                if mc.get("auto_approve"):
                    self.authorized.add(cid)
                else:
                    self.pending.add(cid)
                result = {"state": "authorizing", "clientid": cid, "app_id": params.get("app_id", "")}
            else:
                return
        self.printer.event("mqtt_pairing", {"method": method, "clientid": cid, "state": result["state"]}, transport="mqtt",
                           path=topic)
        session.broker.route(code + "/config/response", json.dumps({"jsonrpc": "2.0", "id": req.get("id"),
                                                                    "result": result}).encode())

    def _lan_message(self, session, topic, payload):
        if not topic.endswith("/request"):
            return
        sn = topic[: -len("/request")]
        try:
            req = json.loads(payload)
        except ValueError:
            return
        method, params = req.get("method", ""), req.get("params") or {}
        with self.printer.lock:
            if method == "printer.objects.query":
                spec = [(k, v or None) for k, v in (params.get("objects") or {}).items()]
                result = self.query_objects(spec)["result"]
            elif method == "server.info":
                result = {"klippy_connected": True, "klippy_state": "ready", "via": "lan"}
            elif method == "printer.gcode.script":
                from core.http import Request
                fake = Request("MQTT", "/printer/gcode/script", "", {}, b"", 0, "mqtt")
                r = self.handle_script(fake, params.get("script", ""))
                result = "ok" if r.status == 200 else None
                if result is None:
                    self.printer.event("mqtt_rpc", {"method": method, "error": r.body}, transport="mqtt")
                    session.broker.route(sn + "/response", json.dumps({"jsonrpc": "2.0", "id": req.get("id"),
                                                                       "error": r.body.get("error")}).encode())
                    return
            else:
                result = {"via": "lan", "method": method}
        self.printer.event("mqtt_rpc", {"method": method, "params": params}, transport="mqtt", path=topic)
        session.broker.route(sn + "/response", json.dumps({"jsonrpc": "2.0", "id": req.get("id"), "result": result}).encode())

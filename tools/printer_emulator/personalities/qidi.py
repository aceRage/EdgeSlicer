"""QIDI on Moonraker (X-Max 4, X-Plus 4/5, Q2 / Q2C), with or without a QIDI Box (design 2.1).

What the slicer reads and writes, and where this emulator keeps it:

  multi_color_controller   box_count, and the slots in one of the two shapes QIDIStudio has read
                           (qidi.mcc_shape): "array" = boxes[b].filament_info[4] (filament_type,
                           filament_str, vendor_str, color_rgb_str[]) + boxes[b].box_inserted_<n>
                           (its source up to 2026-10-09); "flat" = box<b>_box_inserted_<n>,
                           box<b>_filament_info_<n>_filament_type ... (its 2026-10-19 source).
                           box0 is Box 1. UNVERIFIED against a firmware capture (design section 8).
  save_variables           Klipper's [save_variables], laid out as the firmware's own
                           saved_variables.cfg ships (QIDITECH/QIDI_MAX4 config/saved_variables
                           .cfg.bak): filament_slot<n> / color_slot<n> / vendor_slot<n> for n = 0..16
                           all default to 1 (a PLA, white) and are NOT cleared when a spool is
                           pulled; slot<n> (n = 0..15) is the slot's state word, 0 empty, 1 there;
                           runout_<n>; box_count. Box slot n = box * 4 + slot (Box 1 = slots 0-3).
                           And the variables the start script writes: enable_box, value_t<tool>
                           ('slot<n>'), enable_polar_cooler.
  box_stepper slot<n>      one object per physical Box slot (box_config box0 -> slot0..3), with
                           runout_button 0 when filament is at the slot's sensor - what QIDIStudio
                           2.07.02.60 reads as "this slot has material". A query for a stepper the
                           printer does not have answers {} (Klipper's webhooks do that).
  output_pin polar_cooler  present when the model has QIDIStudio's polar cooler.
  officiall_filas_list.cfg /server/files/config/officiall_filas_list.cfg: [fila<id>] type= /
                           filament=, [colordict] <id> = #RRGGBB. SYNTHETIC - generated from a
                           generic table plus the configured slots; QIDI's own file is not copied.

The script interpreter really runs SAVE_VARIABLE (Klipper shlex + literal_eval, klipper.py), so
the read-back returns what was stored - or, with the drop_save_variable fault, what was there
before. That read-back is the step that catches the #611 class of bug.
"""
from core.http import Response, moonraker_error
from .moonraker import FAULTS as MOONRAKER_FAULTS, Moonraker, fault_on
from . import klipper

QIDI_FAULTS = MOONRAKER_FAULTS + [
    {"key": "drop_save_variable", "label": "Drop SAVE_VARIABLE (answer ok, store nothing)", "type": "bool",
     "help": "The #611 guard: the read-back must then stop the send - no upload, no start."},
    {"key": "refuse_g31", "label": "Refuse G31 (fresh bed mesh)", "type": "bool",
     "help": "G31 answers a Klipper error. The print must still go ahead."},
    {"key": "refuse_g32", "label": "Refuse G32 (saved bed mesh)", "type": "bool", "help": ""},
    {"key": "box_unplugged", "label": "Box unplugged (box_count 0)", "type": "bool", "help": ""},
    {"key": "box_unreadable", "label": "Box unreadable (multi_color_controller without filament)", "type": "bool",
     "help": "The slicer then reads save_variables and officiall_filas_list.cfg, as QIDIStudio does."},
    {"key": "filas_cfg_404", "label": "officiall_filas_list.cfg 404", "type": "bool", "help": ""},
]

# Synthetic filament list: generic material names, ids picked here. Not QIDI's file.
BASE_FILAS = [(1, "PLA", "PLA"), (2, "PLA", "PLA Matte"), (3, "PLA", "PLA Silk"), (4, "PLA", "PLA-CF"),
              (5, "PETG", "PETG"), (6, "PETG", "PETG-CF"), (7, "ABS", "ABS"), (8, "ASA", "ASA"),
              (9, "TPU", "TPU 95A"), (10, "PA", "PA12-CF"), (11, "PC", "PC"), (12, "PET", "PET-CF")]
BASE_COLOURS = ["#FFFFFF", "#000000", "#E03030", "#1E7A3C", "#2050C0", "#F0D020", "#F08020", "#808080",
                "#8040A0", "#F5F5F5", "#202020", "#C0C0C0"]
BOX_REPORTS = ("mcc+vars", "vars_only", "mcc_only")
MCC_SHAPES = ("array", "flat")


def norm_colour(c):
    c = (c or "").strip().upper()
    if c and not c.startswith("#"):
        c = "#" + c
    return c[:7] if len(c) >= 7 else ""


class Qidi(Moonraker):
    name = "qidi"
    label = "QIDI (Moonraker, QIDI Box)"
    host_type_hint = "Octo/Klipper"
    FAULTS = QIDI_FAULTS

    # ---- config and state -------------------------------------------------------------------
    @property
    def qcfg(self):
        return self.cfg.get("qidi") or {}

    def boxes(self):
        if fault_on(self.faults, "box_unplugged"):
            return 0
        try:
            return max(0, min(4, int(self.qcfg.get("boxes", 1))))
        except (TypeError, ValueError):
            return 0

    def box_report(self):
        r = self.qcfg.get("box_report", "mcc+vars")
        return r if r in BOX_REPORTS else "mcc+vars"

    def mcc_shape(self):
        r = self.qcfg.get("mcc_shape", "array")
        return r if r in MCC_SHAPES else "array"

    def _int_cfg(self, key, default):
        v = self.qcfg.get(key)
        if v is None:
            return default
        try:
            return max(0, min(4, int(v)))
        except (TypeError, ValueError):
            return default

    def vars_box_count(self):
        """box_count as save_variables keep it: the Boxes, unless qidi.vars_box_count overrides it
        (a stale count, the 2.4.6.3 report's shape)."""
        return 0 if self.boxes() == 0 else self._int_cfg("vars_box_count", self.boxes())

    def stepper_boxes(self):
        """How many Boxes the firmware registered slot motors for (box_stepper slot<n>)."""
        return 0 if self.boxes() == 0 else self._int_cfg("stepper_boxes", self.boxes())

    def slots(self):
        """The configured Box slots, padded to boxes * 4."""
        n = self.boxes() * 4
        s = [dict(x) if isinstance(x, dict) else {} for x in (self.cfg.get("slots") or [])][:n]
        while len(s) < n:
            s.append({"loaded": False})
        return s

    def reset(self):
        super().reset()
        self.bedmesh_before_print = None
        v = {}
        en = self.qcfg.get("enable_box")
        v["enable_box"] = int(en) if en is not None else (1 if self.boxes() > 0 else 0)
        for tool, slot in (self.qcfg.get("stale_value_t") or {}).items():
            v["value_t%d" % int(tool)] = str(slot)
        v["enable_polar_cooler"] = 0
        self.variables = v
        self.refresh_box_variables()

    def config_changed(self, old):
        super().config_changed(old)
        self.refresh_box_variables()

    def filament_list(self):
        """-> (filas [(id, type, name)], colours {id: '#RRGGBB'}): the base table plus whatever the
        configured slots name that it lacks."""
        filas = list(BASE_FILAS)
        colours = {i + 1: c for i, c in enumerate(BASE_COLOURS)}
        for s in self.slots():
            if not s.get("loaded"):
                continue
            if s.get("untagged"):
                continue
            t, sub = (s.get("type") or "").strip(), (s.get("sub") or "").strip()
            if t and self._fila_id(filas, t, sub) == 0:
                filas.append((100 + len(filas), t, sub or t))
            c = norm_colour(s.get("color"))
            if c and c not in colours.values():
                colours[100 + len(colours)] = c
        return filas, colours

    @staticmethod
    def _fila_id(filas, t, sub):
        if sub:
            for i, ft, fn in filas:
                if fn.lower() == sub.lower() and ft.lower() == t.lower():
                    return i
        for i, ft, fn in filas:
            if ft.lower() == t.lower() and (not sub or fn.lower() == sub.lower() or fn.lower() == ft.lower()):
                return i
        return 0

    @staticmethod
    def _is_slot_word(k):
        return k.startswith("slot") and k[4:].isdigit()

    def refresh_box_variables(self):
        """The Box's save_variables as the firmware keeps them (see the module docstring): every
        filament_slot / color_slot / vendor_slot 0..16 at the shipped default 1 (qidi.fw_defaults,
        on unless set false), a loaded slot's ids from its material and colour (0 for an untagged
        spool), an empty slot's ids left at the default (or at qidi slot's `stale_type` /
        `stale_color`, what the last spool there was) and its state word slot<n> 0. The variables
        the slicer writes (enable_box, value_t<n>, enable_polar_cooler) are kept."""
        for k in list(self.variables):
            if k == "box_count" or k.startswith(("filament_slot", "color_slot", "vendor_slot", "runout_")) or \
                    self._is_slot_word(k):
                del self.variables[k]
        if self.box_report() == "mcc_only":
            return
        filas, colours = self.filament_list()
        default = 1 if self.qcfg.get("fw_defaults", True) else 0
        for n in range(17):
            self.variables["filament_slot%d" % n] = default
            self.variables["color_slot%d" % n] = default
            self.variables["vendor_slot%d" % n] = default
            if n < 16:
                self.variables["slot%d" % n] = 0
                self.variables["runout_%d" % n] = 0
        self.variables["box_count"] = self.vars_box_count()
        for n, s in enumerate(self.slots()):
            loaded = bool(s.get("loaded"))
            t = (s.get("type") if loaded else s.get("stale_type")) or ""
            c = norm_colour(s.get("color") if loaded else s.get("stale_color"))
            if loaded:
                self.variables["slot%d" % n] = 1
            if loaded and s.get("untagged"):
                fid, cid = 0, 0
            elif t.strip():
                fid = self._fila_id(filas, t.strip(), (s.get("sub") or "").strip() if loaded else "")
                cid = next((i for i, h in colours.items() if h == c), 0)
            elif loaded:
                fid, cid = 0, 0
            else:
                continue  # an empty slot keeps whatever the firmware had there
            self.variables["filament_slot%d" % n] = fid
            self.variables["color_slot%d" % n] = cid

    def snapshot(self):
        s = super().snapshot()
        s["save_variables"] = {k: self.variables[k] for k in sorted(self.variables)
                               if k in ("enable_box", "enable_polar_cooler", "box_count") or k.startswith("value_t")}
        s["bedmesh_before_print"] = self.bedmesh_before_print
        s["boxes"] = self.boxes()
        s["box_steppers"] = self.stepper_boxes() * 4
        return s

    def hint(self):
        h = super().hint()
        h["printer_preset"] = self.cfg.get("edgeslicer_preset") or "Qidi X-Max 4 0.4 nozzle"
        return h

    # ---- objects ----------------------------------------------------------------------------
    def objects(self):
        objs = super().objects()
        nb = self.boxes()
        if nb == 0:
            objs["multi_color_controller"] = {"box_count": 0}
        elif self.box_report() == "vars_only" or fault_on(self.faults, "box_unreadable"):
            # The controller is there and counts the Box, but names no filament.
            objs["multi_color_controller"] = {"box_count": nb}
        elif self.mcc_shape() == "flat":
            mcc = {"box_count": nb}
            slots = self.slots()
            for b in range(nb):
                pre = "box%d_" % b
                mcc[pre + "aht20_temp"] = 25.0
                mcc[pre + "aht20_humidity"] = 30
                for n in range(4):
                    s = slots[b * 4 + n]
                    loaded = bool(s.get("loaded"))
                    mcc[pre + "box_inserted_%d" % n] = 1 if loaded else 0
                    if loaded and not s.get("untagged"):
                        fi = pre + "filament_info_%d_" % n
                        mcc[fi + "filament_type"] = s.get("type", "")
                        mcc[fi + "filament_str"] = s.get("sub") or s.get("type", "")
                        mcc[fi + "vendor_str"] = s.get("vendor", "")
                        mcc[fi + "color_rgb_str"] = [norm_colour(s.get("color"))]
            objs["multi_color_controller"] = mcc
        else:
            boxes = []
            slots = self.slots()
            for b in range(nb):
                box = {"filament_info": [], "temperature": 25.0, "humidity": 30}
                for n in range(4):
                    s = slots[b * 4 + n]
                    loaded = bool(s.get("loaded"))
                    box["box_inserted_%d" % n] = loaded
                    if loaded and not s.get("untagged"):
                        box["filament_info"].append({"filament_type": s.get("type", ""),
                                                     "filament_str": s.get("sub") or s.get("type", ""),
                                                     "vendor_str": s.get("vendor", ""),
                                                     "color_rgb_str": [norm_colour(s.get("color"))]})
                    else:
                        box["filament_info"].append({})
                boxes.append(box)
            objs["multi_color_controller"] = {"box_count": nb, "boxes": boxes}
        # The slot motors: one box_stepper per slot of every Box the firmware registered, with the
        # slot's sensor (runout_button 0 = filament there).
        slots = self.slots()
        for n in range(self.stepper_boxes() * 4):
            loaded = n < len(slots) and bool(slots[n].get("loaded"))
            objs["box_stepper slot%d" % n] = {"runout_button": 0 if loaded else 1}
        objs["save_variables"] = {"variables": dict(self.variables)}
        if self.qcfg.get("polar_cooler"):
            objs["output_pin polar_cooler"] = {"value": float(self.variables.get("enable_polar_cooler", 0) or 0)}
        return objs

    def query_objects(self, spec):
        """As Moonraker's, but a box_stepper the printer does not have answers {} - Klipper's
        webhooks answer an unknown object that way, and a reader must not count it as a slot."""
        out = super().query_objects(spec)
        status = out["result"]["status"]
        for name, _attrs in spec:
            if name.startswith("box_stepper ") and name not in status:
                status[name] = {}
        return out

    def query_kind(self, names):
        if "multi_color_controller" in names or "print_task_config" in names:
            return "probe"
        if names == ["save_variables"]:
            return "readback"
        return "query"

    def config_file(self, name):
        if name != "officiall_filas_list.cfg":
            return super().config_file(name)
        if fault_on(self.faults, "filas_cfg_404"):
            return moonraker_error(404, "File not found: config/" + name, kind="filas_cfg",
                                   payload={"file": name, "refused": True, "fault": "filas_cfg_404"})
        filas, colours = self.filament_list()
        lines = ["# EdgeSlicer printer emulator: a SYNTHETIC officiall_filas_list.cfg (not QIDI's file).", ""]
        for i, t, n in filas:
            lines += ["[fila%d]" % i, "filament = %s" % n, "type = %s" % t, ""]
        lines.append("[colordict]")
        lines += ["%d = %s" % (i, c) for i, c in sorted(colours.items())]
        return Response(200, "\n".join(lines) + "\n", ctype="text/plain", kind="filas_cfg", payload={"file": name})

    # ---- the interpreter --------------------------------------------------------------------
    def gcode_help(self):
        h = super().gcode_help()
        h.update({"G31": "Probe a fresh bed mesh before the print", "G32": "Use the saved bed mesh"})
        return h

    def run_command(self, req, cmd, params, line):
        if cmd == "SAVE_VARIABLE":
            name, value, raw = klipper.save_variable_args(params)
            drop = fault_on(self.faults, "drop_save_variable")
            if not drop:
                self.variables[name] = value
            payload = {"var": name, "value": value, "raw": raw, "line": line.strip(), "stored": not drop}
            if drop:
                payload["fault"] = "drop_save_variable"
            self.printer.sub_log(req, "save_variable", payload)
            return
        if cmd in ("G31", "G32"):
            if fault_on(self.faults, "refuse_g31" if cmd == "G31" else "refuse_g32"):
                self.printer.sub_log(req, "gcode", {"cmd": cmd, "line": line.strip(), "refused": True,
                                                    "fault": "refuse_g31" if cmd == "G31" else "refuse_g32"},
                                     status=400)
                raise klipper.GcodeError("%s refused (emulator fault %s)" % (cmd, "refuse_g31" if cmd == "G31" else "refuse_g32"))
            self.bedmesh_before_print = cmd == "G31"
            self.printer.sub_log(req, "gcode", {"cmd": cmd, "line": line.strip(),
                                                "meaning": "probe a fresh mesh before the print" if cmd == "G31"
                                                else "load the saved mesh"})
            return
        super().run_command(req, cmd, params, line)

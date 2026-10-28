"""The supervisor: printers.json, the presets, and the printers it runs."""
import copy
import json
import os
import threading

from personalities import personality_class
from . import safety
from .log import CommandLog
from .printer import EmulatedPrinter
from .store import UploadStore

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRESET_DIR = os.path.join(HERE, "presets")
# The printers a first start creates. A printers.json from an earlier phase gets the ones it never
# had added once (its "seeded" list remembers them, so a printer the owner removed stays removed).
DEFAULT_PRINTERS = ["qidi_xmax4_box", "qidi_q2_nobox", "elegoo_cc1_canvas", "u1_4head"]
E1_DEFAULTS = ["qidi_xmax4_box", "qidi_q2_nobox"]


def deep_merge(dst, patch):
    """`patch` into `dst`; a None value removes the key; lists are replaced whole."""
    for k, v in patch.items():
        if v is None:
            dst.pop(k, None)
        elif isinstance(v, dict) and isinstance(dst.get(k), dict) and k != "http_error":
            deep_merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)
    return dst


def load_presets():
    out = {}
    try:
        names = sorted(os.listdir(PRESET_DIR))
    except OSError:
        return out
    for n in names:
        if n.endswith(".json"):
            try:
                with open(os.path.join(PRESET_DIR, n), encoding="utf-8") as f:
                    out[n[:-5]] = json.load(f)
            except (OSError, ValueError):
                pass
    return out


class Supervisor:
    def __init__(self, home, config_path=None, save=True):
        self.home = home
        os.makedirs(home, exist_ok=True)
        self.config_path = config_path or os.path.join(home, "printers.json")
        self.default_config = config_path is None
        self.save_enabled = save
        self.lock = threading.RLock()
        self.log = CommandLog(os.path.join(home, "logs"))
        self.store = UploadStore(os.path.join(home, "uploads"))
        self.presets = load_presets()
        self.printers = {}
        self.config = self._load_config()

    # ---- config file ------------------------------------------------------------------------
    def _load_config(self):
        if os.path.exists(self.config_path):
            with open(self.config_path, encoding="utf-8") as f:
                cfg = json.load(f)
            cfg.setdefault("printers", [])
            if self.default_config:
                # Only the owner's own printers.json (never a gate's --config) grows new defaults.
                seeded = cfg.setdefault("seeded", list(E1_DEFAULTS))
                ids = {p.get("id") for p in cfg["printers"]}
                added = False
                for n in DEFAULT_PRINTERS:
                    if n in seeded or n not in self.presets:
                        continue
                    seeded.append(n)
                    added = True
                    if self.presets[n].get("id") not in ids:
                        cfg["printers"].append(copy.deepcopy(self.presets[n]))
                if added:
                    self.config = cfg
                    self._write(cfg)
        else:
            cfg = {"printers": [copy.deepcopy(self.presets[n]) for n in DEFAULT_PRINTERS if n in self.presets],
                   "seeded": list(DEFAULT_PRINTERS)}
            self.config = cfg
            self._write(cfg)
        cfg.setdefault("printers", [])
        return cfg

    def _write(self, cfg):
        if not self.save_enabled:
            return
        tmp = self.config_path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(cfg, f, indent=2)
            f.write("\n")
        os.replace(tmp, self.config_path)

    def save(self):
        with self.lock:
            self._write(self.config)

    # ---- printers ---------------------------------------------------------------------------
    def start_all(self):
        for entry in self.config["printers"]:
            self._spawn(entry)

    def _spawn(self, entry):
        if not entry.get("id"):
            raise ValueError("a printer needs an id")
        cls = personality_class(entry.get("personality", "moonraker"))
        p = EmulatedPrinter(entry, cls, self.log, self.store)
        self.printers[entry["id"]] = p
        p.reconcile()
        return p

    def printer_list(self):
        """A snapshot of the printers, taken under the lock: the control page polls while printers are
        added or removed on other threads, and iterating the live dict then raises."""
        with self.lock:
            return list(self.printers.values())

    def get(self, pid):
        p = self.printers.get(pid)
        if p is None:
            raise KeyError("no printer '%s'" % pid)
        return p

    def add(self, preset=None, entry=None, overrides=None):
        with self.lock:
            if preset:
                if preset not in self.presets:
                    raise KeyError("no preset '%s'" % preset)
                entry = copy.deepcopy(self.presets[preset])
            entry = copy.deepcopy(entry or {})
            deep_merge(entry, overrides or {})
            base = entry.get("id") or preset or "printer"
            pid, n = base, 2
            while pid in self.printers:
                pid, n = "%s-%d" % (base, n), n + 1
            entry["id"] = pid
            if overrides is None or "bind" not in overrides:
                entry["bind"] = self._free_alias(entry.get("bind", "127.0.0.11"), int(entry.get("port", 7125)))
            safety.check_bind(entry.get("bind"), bool(entry.get("lan")))
            self.config["printers"].append(entry)
            p = self._spawn(entry)
            self.save()
            return p

    def _free_alias(self, start, port):
        """A 127.0.0.x no other emulated printer uses with this port (fixed-port protocols need one
        address each; design 3.2)."""
        used = {(q.cfg.get("bind"), int(q.cfg.get("port", 0))) for q in self.printer_list()}
        if (start, port) not in used:
            return start
        if not safety.is_loopback(start):
            return start
        a, b, c, d = (int(x) for x in start.split("."))
        for x in range(max(d, 11), 255):
            cand = "%d.%d.%d.%d" % (a, b, c, x)
            if (cand, port) not in used:
                return cand
        return start

    def remove(self, pid):
        with self.lock:
            p = self.get(pid)
            p.stop()
            del self.printers[pid]
            self.config["printers"] = [e for e in self.config["printers"] if e.get("id") != pid]
            self.save()

    def set(self, pid, patch):
        """Apply a partial config to one printer. Reopens its listeners when the address, port,
        enablement or a listener fault changed."""
        with self.lock:
            p = self.get(pid)
            if "id" in patch and patch["id"] != pid:
                raise ValueError("the id cannot be changed")
            if "personality" in patch and patch["personality"] != p.cfg.get("personality"):
                raise ValueError("the personality cannot be changed; add a new printer instead")
            new = deep_merge(copy.deepcopy(p.cfg), patch)
            safety.check_bind(new.get("bind", "127.0.0.1"), bool(new.get("lan")))
            old = copy.deepcopy(p.cfg)
            with p.lock:
                p.cfg.clear()
                p.cfg.update(new)
                p.personality.config_changed(old)
            if "faults" in patch and not (p.cfg.get("faults") or {}).get("timeout"):
                p.release.set()
            p.reconcile()
            self.save()
            return p

    def reset(self, pid, patch=None):
        """Forget the printer's runtime state (files, prints, saved variables) after an optional
        config patch. Returns the log sequence number to read from afterwards."""
        with self.lock:
            p = self.set(pid, patch) if patch else self.get(pid)
            with p.lock:
                p.personality.reset()
            mark = self.log.mark()
            self.log.add(pid, p.personality.name, "reset", transport="control", payload={"patch": patch or {}},
                         state_after=p.snapshot())
            return mark

    def set_state(self, pid, state):
        with self.lock:
            p = self.get(pid)
            with p.lock:
                p.cfg["state"] = state
                p.personality.set_state(state)
            self.log.add(pid, p.personality.name, "state", transport="control", payload={"state": state},
                         state_after=p.snapshot())
            self.save()

    def control(self, pid, action):
        with self.lock:
            p = self.get(pid)
            if action == "start":
                p.cfg["enabled"] = True
                p.reconcile()
            elif action == "stop":
                p.cfg["enabled"] = False
                p.stop()
            elif action == "restart":
                p.stop()
                p.cfg["enabled"] = True
                p.reconcile()
            else:
                raise ValueError("unknown action " + action)
            self.save()
            return p

    def stop_all(self):
        for p in self.printer_list():
            p.stop()
        self.log.close()

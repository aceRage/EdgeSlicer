"""One emulated printer: its config entry, its personality, its listeners and the faults every
personality shares (timeout, connection refused, HTTP error per path)."""
import threading
import time

from . import safety
from .http import Listener, Response, moonraker_error


class EmulatedPrinter:
    def __init__(self, cfg, personality_cls, log, store):
        self.cfg = cfg
        self.log = log
        self.store = store
        self.lock = threading.RLock()
        self.release = threading.Event()
        self.listeners = {}
        self.error = ""
        self.personality = None
        self.personality_cls = personality_cls
        with self.lock:
            self.personality = personality_cls(self)

    @property
    def id(self):
        return self.cfg["id"]

    @property
    def faults(self):
        return self.cfg.get("faults") or {}

    # ---- listeners --------------------------------------------------------------------------
    def wanted_ports(self):
        if not self.cfg.get("enabled", True) or self.faults.get("refuse_connections"):
            return []
        return self.personality.listen_ports()

    def reconcile(self):
        """Open the listeners the config wants, close the others. Sets self.error on a bind
        failure (with who holds the port, and the Mac alias hint)."""
        with self.lock:
            host = self.cfg.get("bind", "127.0.0.1")
            try:
                safety.check_bind(host, bool(self.cfg.get("lan")))
            except safety.BindRefused as e:
                for key in list(self.listeners):
                    self.listeners.pop(key).down()
                self.error = str(e)
                return False
            want = {(host, p) for p in self.wanted_ports()}
            for key in list(self.listeners):
                if key not in want:
                    self.listeners.pop(key).down()
            errors = []
            for key in sorted(want):
                if key in self.listeners:
                    continue
                lst = Listener(self, key[0], key[1])
                try:
                    lst.up()
                    self.listeners[key] = lst
                except OSError as e:
                    holder = safety.who_holds(key[1])
                    errors.append("cannot listen on %s:%d (%s)%s%s" % (key[0], key[1], e.strerror or e,
                                                                       "; held by " + holder if holder else "",
                                                                       safety.mac_alias_hint(key[0])))
            self.error = "; ".join(errors)
            if not want:
                self.release.set()  # anything held by the timeout fault lets go
            return not errors

    def stop(self):
        with self.lock:
            for key in list(self.listeners):
                self.listeners.pop(key).down()
            self.release.set()

    @property
    def running(self):
        return bool(self.listeners)

    def addresses(self):
        return ["%s:%d" % k for k in sorted(self.listeners)]

    # ---- requests ---------------------------------------------------------------------------
    def handle(self, req):
        if self.faults.get("timeout"):
            self.release.clear()
            return Response(0, None, kind="fault", payload={"fault": "timeout", "held": True}, hold=True)
        errs = self.faults.get("http_error") or {}
        if isinstance(errs, dict) and req.path in errs:
            try:
                code = int(errs[req.path])
            except (TypeError, ValueError):
                code = 500
            return moonraker_error(code, "emulated HTTP %d (fault http_error)" % code, kind="fault",
                                   payload={"fault": "http_error", "code": code})
        with self.lock:
            return self.personality.route(req)

    def hold_until_released(self, max_s=600):
        """The timeout fault: keep the connection, answer nothing, until the fault is switched off,
        the printer stops, or ten minutes pass."""
        t0 = time.time()
        while time.time() - t0 < max_s:
            if self.release.wait(0.5) or not self.faults.get("timeout"):
                return

    def sleep_interruptible(self, seconds):
        """A slow answer that the printer stopping cuts short. Called with the lock held, so the
        lock is let go while waiting."""
        self.lock.release()
        try:
            self.release.clear()
            self.release.wait(seconds)
        finally:
            self.lock.acquire()

    # ---- logging ----------------------------------------------------------------------------
    def snapshot(self):
        with self.lock:
            return self.personality.snapshot()

    def record(self, req, resp, ms, status):
        self.log.add(self.id, self.personality.name, resp.kind, method=req.method, path=req.path, query=req.query or None,
                     port=req.port, status=status, ms=ms, payload=resp.payload, state_after=self.snapshot())

    def sub_log(self, req, kind, payload, status=200):
        """A decoded sub-command of the request being handled (each SAVE_VARIABLE of a script, the
        start an upload carried)."""
        self.log.add(self.id, self.personality.name, kind, method=req.method, path=req.path, port=req.port,
                     status=status, payload=payload, state_after=self.personality.snapshot())

    # ---- for the control page ---------------------------------------------------------------
    def describe(self):
        with self.lock:
            p = self.personality
            return {"id": self.id, "config": self.cfg, "personality": p.name, "label": p.label,
                    "running": self.running, "addresses": self.addresses(), "error": self.error,
                    "state": p.snapshot(), "hint": p.hint(), "faults": p.FAULTS}

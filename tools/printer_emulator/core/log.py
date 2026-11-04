"""The command log: every request a printer got, decoded, with a state snapshot after it.

One entry per request (and one per decoded sub-command, such as each SAVE_VARIABLE of a script):

    {"seq": 57, "ts": "2026-10-20T21:14:03.120Z", "printer": "qidi-xmax4", "personality": "qidi",
     "transport": "http", "method": "POST", "path": "/printer/gcode/script", "port": 7125,
     "status": 200, "ms": 12, "kind": "save_variable",
     "payload": {"var": "value_t0", "value": "slot3", "raw": "\"'slot3'\"", "stored": true},
     "state_after": {...}}

Kept three ways: in memory (for /emu/log and the assertions), pushed to live subscribers (the
control page's server-sent events), and appended to a JSONL file per printer per session under
<home>/logs (design D6: the last 20 sessions are kept).
"""
import json
import os
import queue
import threading
import time
from collections import deque

KEEP_SESSIONS = 20


def utc_stamp(t=None):
    t = time.time() if t is None else t
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % int((t % 1) * 1000)


class CommandLog:
    def __init__(self, log_dir, keep_sessions=KEEP_SESSIONS, max_entries=20000):
        self.lock = threading.Lock()
        self.seq = 0
        self.entries = deque(maxlen=max_entries)
        self.subscribers = []
        self.session = time.strftime("%Y%m%d-%H%M%S")
        self.dir = log_dir
        self.files = {}
        self.keep_sessions = keep_sessions
        if self.dir:
            os.makedirs(self.dir, exist_ok=True)
            self.prune()

    # ---- writing ---------------------------------------------------------------------------
    def add(self, printer, personality, kind, transport="http", method=None, path=None, port=None,
            status=None, ms=None, payload=None, state_after=None, query=None):
        entry = {"seq": 0, "ts": utc_stamp(), "printer": printer, "personality": personality,
                 "transport": transport, "kind": kind}
        if method is not None:
            entry["method"] = method
        if path is not None:
            entry["path"] = path
        if query:
            entry["query"] = query
        if port is not None:
            entry["port"] = port
        if status is not None:
            entry["status"] = status
        if ms is not None:
            entry["ms"] = ms
        entry["payload"] = payload if payload is not None else {}
        if state_after is not None:
            entry["state_after"] = state_after
        with self.lock:
            self.seq += 1
            entry["seq"] = self.seq
            self.entries.append(entry)
            self._write(printer, entry)
            subs = list(self.subscribers)
        for q in subs:
            try:
                q.put_nowait(entry)
            except queue.Full:
                pass  # a stalled page drops lines rather than stall the printer
        return entry

    def _write(self, printer, entry):
        if not self.dir:
            return
        f = self.files.get(printer)
        if f is None:
            safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in printer or "emulator")
            try:
                f = open(os.path.join(self.dir, "%s_%s.jsonl" % (self.session, safe)), "a", encoding="utf-8",
                         newline="\n")
            except OSError:
                return
            self.files[printer] = f
        try:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
            f.flush()
        except (OSError, ValueError):
            pass

    # ---- reading ---------------------------------------------------------------------------
    def mark(self):
        with self.lock:
            return self.seq

    def query(self, printer=None, since=0, kind=None, limit=5000):
        with self.lock:
            out = [e for e in self.entries
                   if e["seq"] > since and (not printer or e["printer"] == printer) and (not kind or e["kind"] == kind)]
        return out[-limit:] if limit else out

    def subscribe(self):
        q = queue.Queue(maxsize=2000)
        with self.lock:
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subscribers:
                self.subscribers.remove(q)

    def session_files(self):
        with self.lock:
            return [f.name for f in self.files.values()]

    def close(self):
        with self.lock:
            for f in self.files.values():
                try:
                    f.close()
                except OSError:
                    pass
            self.files = {}

    # ---- retention -------------------------------------------------------------------------
    def prune(self):
        """Keep the newest `keep_sessions` sessions (this one included); a session is every file
        sharing one start stamp."""
        try:
            names = [n for n in os.listdir(self.dir) if n.endswith(".jsonl") and "_" in n]
        except OSError:
            return
        stamps = sorted({n.split("_", 1)[0] for n in names}, reverse=True)
        keep = set(stamps[: max(0, self.keep_sessions - 1)]) | {self.session}
        for n in names:
            if n.split("_", 1)[0] not in keep:
                try:
                    os.remove(os.path.join(self.dir, n))
                except OSError:
                    pass

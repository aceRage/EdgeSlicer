"""The control page and API. Always on 127.0.0.1 (safety rail 5), whatever the printers bind to.

  GET  /                         the control page (page/index.html)
  GET  /emu/ping                 {"emulator": true, "version": ...}
  GET  /emu/printers             every printer: config, runtime state, addresses, faults, hint
  GET  /emu/presets              the preset names and entries
  GET  /emu/state?printer=       one printer's state snapshot
  GET  /emu/log?printer=&since=&kind=&limit=   log entries after `since`
  GET  /emu/mark                 the current log sequence number
  GET  /emu/events               server-sent events: one `data:` line per new log entry
  GET  /emu/filas?printer=       the synthetic officiall_filas_list.cfg a QIDI printer serves
  GET  /emu/bundle               a zip of this session's logs, printers.json and the upload list
  POST /emu/printers/add         {"preset": name, "overrides": {...}} or {"entry": {...}}
  POST /emu/printers/<id>/set    JSON patch (deep merge; null removes a key)
  POST /emu/printers/<id>/reset  optional JSON patch, then runtime state forgotten; -> {"mark": seq}
  POST /emu/printers/<id>/state  {"state": "standby|printing|paused|..."}
  POST /emu/printers/<id>/start | stop | restart | remove
  POST /emu/quit                 stop every printer and exit
"""
import io
import json
import os
import threading
import time
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, urlparse

from . import safety

VERSION = "E2"
PAGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "page", "index.html")


class ControlHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "EmuControl/1"

    def log_message(self, fmt, *a):
        pass

    @property
    def sup(self):
        return self.server.sup

    def send(self, code, obj, ctype="application/json", extra=None):
        data = obj if isinstance(obj, (bytes, bytearray)) else (
            obj.encode("utf-8") if isinstance(obj, str) else json.dumps(obj, default=str).encode("utf-8"))
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if not raw:
            return {}
        v = json.loads(raw)
        if not isinstance(v, dict):
            raise ValueError("the body must be a JSON object")
        return v

    def do_GET(self):
        u = urlparse(self.path)
        q = dict(parse_qsl(u.query))
        p = u.path
        sup = self.sup
        try:
            if p in ("/", "/index.html"):
                with open(PAGE, "rb") as f:
                    return self.send(200, f.read(), "text/html; charset=utf-8")
            if p == "/emu/ping":
                return self.send(200, {"emulator": True, "version": VERSION, "home": sup.home,
                                       "session": sup.log.session, "pid": os.getpid()})
            if p == "/emu/printers":
                return self.send(200, {"printers": [x.describe() for x in sup.printer_list()],
                                       "mark": sup.log.mark(), "home": sup.home, "config": sup.config_path})
            if p == "/emu/presets":
                return self.send(200, {"presets": sup.presets})
            if p == "/emu/state":
                return self.send(200, sup.get(q.get("printer", "")).snapshot())
            if p == "/emu/mark":
                return self.send(200, {"mark": sup.log.mark()})
            if p == "/emu/log":
                return self.send(200, {"entries": sup.log.query(q.get("printer") or None, int(q.get("since") or 0),
                                                                q.get("kind") or None, int(q.get("limit") or 5000)),
                                       "mark": sup.log.mark()})
            if p == "/emu/filas":
                pr = sup.get(q.get("printer", ""))
                fn = getattr(pr.personality, "config_file", None)
                if fn is None:
                    return self.send(404, {"error": "not a QIDI printer"})
                r = fn("officiall_filas_list.cfg")
                return self.send(r.status, r.encoded(), r.ctype)
            if p == "/emu/events":
                return self.events(int(q.get("since") or 0))
            if p == "/emu/bundle":
                name, data = bundle(sup)
                return self.send(200, data, "application/zip",
                                 {"Content-Disposition": 'attachment; filename="%s"' % name})
            return self.send(404, {"error": "no such path"})
        except KeyError as e:
            return self.send(404, {"error": str(e).strip("'\"")})
        except (ValueError, OSError) as e:
            return self.send(400, {"error": str(e)})

    def do_POST(self):
        u = urlparse(self.path)
        p = u.path
        sup = self.sup
        try:
            body = self.body_json()
            if p == "/emu/quit":
                self.send(200, {"quitting": True})
                threading.Thread(target=self.server.quit, daemon=True).start()
                return
            if p == "/emu/printers/add":
                pr = sup.add(preset=body.get("preset"), entry=body.get("entry"), overrides=body.get("overrides"))
                return self.send(200, pr.describe())
            if p.startswith("/emu/printers/"):
                rest = p[len("/emu/printers/"):]
                pid, _, action = rest.rpartition("/")
                if action == "set":
                    return self.send(200, sup.set(pid, body).describe())
                if action == "reset":
                    mark = sup.reset(pid, body or None)
                    return self.send(200, {"mark": mark, "printer": sup.get(pid).describe()})
                if action == "state":
                    sup.set_state(pid, body.get("state", "standby"))
                    return self.send(200, sup.get(pid).describe())
                if action in ("start", "stop", "restart"):
                    return self.send(200, sup.control(pid, action).describe())
                if action == "remove":
                    sup.remove(pid)
                    return self.send(200, {"removed": pid})
            return self.send(404, {"error": "no such path"})
        except KeyError as e:
            return self.send(404, {"error": str(e).strip("'\"")})
        except (ValueError, OSError, safety.BindRefused) as e:
            return self.send(400, {"error": str(e)})

    def events(self, since):
        sup = self.sup
        q = sup.log.subscribe()
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            for e in sup.log.query(None, since, None, 500):
                self.wfile.write(("data: %s\n\n" % json.dumps(e, default=str)).encode("utf-8"))
            self.wfile.flush()
            last = time.time()
            while not self.server.quitting:
                try:
                    e = q.get(timeout=1.0)
                    if e["seq"] <= since:
                        continue
                    self.wfile.write(("data: %s\n\n" % json.dumps(e, default=str)).encode("utf-8"))
                    self.wfile.flush()
                    last = time.time()
                except Exception:  # noqa: BLE001 - queue.Empty
                    if time.time() - last > 15:
                        self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                        last = time.time()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            pass
        finally:
            sup.log.unsubscribe(q)


def bundle(sup):
    """The "Save bundle" zip: this session's JSONL logs, printers.json and the uploaded files' list
    (names, sizes, hashes; not the files). Also written to <home>/bundles/."""
    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = "emulator_bundle_%s.zip" % stamp
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sup.log.session_files():
            try:
                z.write(path, "logs/" + os.path.basename(path))
            except OSError:
                pass
        z.writestr("printers.json", json.dumps(sup.config, indent=2))
        uploads = {pr.id: pr.personality.files for pr in sup.printer_list()}
        z.writestr("uploads.json", json.dumps(uploads, indent=2, default=str))
        z.writestr("README.txt", "EdgeSlicer printer emulator bundle, session %s, made %s.\n"
                                 "logs/*.jsonl: one line per request the emulated printer got.\n" % (sup.log.session, stamp))
    data = buf.getvalue()
    try:
        d = os.path.join(sup.home, "bundles")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
    except OSError:
        pass
    return name, data


class ControlServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, sup, port):
        self.sup = sup
        self.quitting = False
        super().__init__((safety.CONTROL_HOST, port), ControlHandler)

    def quit(self):
        self.quitting = True
        time.sleep(0.3)
        self.sup.stop_all()
        self.shutdown()

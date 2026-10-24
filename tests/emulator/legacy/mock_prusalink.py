"""A stand-in PrusaLink printer for the hub-status gate.

Speaks the slice of the PrusaLink REST API that PrusaLinkStatus.cpp asks for, with the auth a real
Buddy board demands:

  GET /api/version    -> { api, server, text: "PrusaLink", hostname, capabilities }
  GET /api/v1/status  -> { printer: { state, temp_nozzle, target_nozzle, temp_bed, target_bed, ... },
                           job: { id, progress, time_remaining, time_printing },   (only mid-job)
                           storage: { ... } }
  GET /api/v1/job     -> { id, state, progress, time_remaining, time_printing,
                           file: { name, display_name, path, size } }              (404 when idle)

Auth, exactly as PrusaLink::set_auth sends it:
  * X-Api-Key: <key>      when the preset's printhost_authorization_type is atKeyPassword
  * HTTP Digest           when it is atUserPassword (qop=auth, MD5, RFC 2617)
Anything without one of the two gets 401 with a WWW-Authenticate: Digest challenge, which is what a
Buddy board does - so the gate can prove the slicer actually authenticates rather than getting lucky.

Its own control surface, for the gate:
  GET  /mock/state              -> everything it is reporting plus the requests it has served
  POST /mock/set?state=PRINTING&progress=62&remaining=1440&file=cube.bgcode&nozzle=215&bed=60
                                -> set what the next poll will see (every parameter optional)
  POST /mock/idle               -> back to IDLE with no job
  POST /mock/reset              -> /mock/idle plus an empty request log
  POST /mock/clearlog           -> empty the request log, leave the reported state alone

Nothing is ever printed: "PRINTING" is a string this process returns.

usage: mock_prusalink.py [port] [apikey]      (runs until killed)
"""
import hashlib, json, os, sys, threading, time, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18095
APIKEY = sys.argv[2] if len(sys.argv) > 2 else "mockprusakey"
# The Digest pair the gate's second preset uses.
DIGEST_USER = os.environ.get("MOCK_PRUSA_USER", "maker")
DIGEST_PASS = os.environ.get("MOCK_PRUSA_PASS", "prusapass")
REALM = "Printer API"
# A fresh opaque/nonce per process: a stale one from a previous run must not be accepted.
NONCE = uuid.uuid4().hex
OPAQUE = uuid.uuid4().hex

LOCK = threading.Lock()
STATE = {
    "state": "IDLE",          # PrusaLink's own vocabulary
    "temp_nozzle": 24.7,
    "target_nozzle": 0.0,
    "temp_bed": 23.9,
    "target_bed": 0.0,
    "progress": None,         # 0..100, None = no job
    "time_remaining": None,   # seconds
    "time_printing": None,
    "file": "",               # display name
    "job_id": 0,
}
REQUESTS = []   # [{"path":..., "auth":"key"|"digest", "at":...}]
MID_JOB = ("PRINTING", "PAUSED")


def md5(s):
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def parse_digest_header(value):
    """`Digest username="x", realm="y", ...` -> dict. Tolerates unquoted values (nc, qop)."""
    if not value or not value.startswith("Digest "):
        return {}
    out = {}
    for part in value[len("Digest "):].split(","):
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        out[k.strip()] = v.strip().strip('"')
    return out


def digest_ok(header, method):
    d = parse_digest_header(header)
    if not d or d.get("username") != DIGEST_USER or d.get("nonce") != NONCE:
        return False
    ha1 = md5("%s:%s:%s" % (DIGEST_USER, REALM, DIGEST_PASS))
    ha2 = md5("%s:%s" % (method, d.get("uri", "")))
    if d.get("qop") in ("auth", "auth-int"):
        want = md5("%s:%s:%s:%s:%s:%s" % (ha1, d.get("nonce", ""), d.get("nc", ""),
                                          d.get("cnonce", ""), d.get("qop"), ha2))
    else:
        want = md5("%s:%s:%s" % (ha1, d.get("nonce", ""), ha2))
    return want == d.get("response")


def job_object(include_file):
    """The job half. `include_file` only for /api/v1/job: /api/v1/status never names the file."""
    with LOCK:
        if STATE["state"] not in MID_JOB or STATE["progress"] is None:
            return None
        j = {"id": STATE["job_id"], "state": STATE["state"], "progress": STATE["progress"]}
        if STATE["time_remaining"] is not None:
            j["time_remaining"] = STATE["time_remaining"]
        if STATE["time_printing"] is not None:
            j["time_printing"] = STATE["time_printing"]
        if include_file and STATE["file"]:
            name = STATE["file"]
            j["file"] = {
                "refs": {"download": "/api/v1/files/usb/" + name},
                "name": name.upper().replace(" ", "~")[:12],
                "display_name": name,
                "path": "/usb",
                "size": 91234,
                "m_timestamp": 1699999999,
            }
        return j


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # ------------------------------------------------------------------ plumbing --
    def _send(self, code, payload, extra_headers=None):
        raw = json.dumps(payload).encode("utf-8") if not isinstance(payload, bytes) else payload
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        for k, v in (extra_headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(raw)

    def _unauthorized(self):
        challenge = 'Digest realm="%s", qop="auth", nonce="%s", opaque="%s"' % (REALM, NONCE, OPAQUE)
        self._send(401, {"title": "Unauthorized", "message": "Missing or invalid API key"},
                   {"WWW-Authenticate": challenge})

    def _authed(self):
        """Returns "key" | "digest" | None, recording which one the slicer used."""
        key = self.headers.get("X-Api-Key")
        if key is not None:
            return "key" if key == APIKEY else None
        auth = self.headers.get("Authorization")
        if auth and auth.startswith("Digest ") and digest_ok(auth, self.command):
            return "digest"
        return None

    # ---------------------------------------------------------------------- GET --
    def do_GET(self):
        u = urlparse(self.path)
        path = u.path.rstrip("/") or "/"

        if path == "/mock/state":
            with LOCK:
                return self._send(200, {"printer": dict(STATE), "requests": list(REQUESTS),
                                        "apikey": APIKEY, "digest_user": DIGEST_USER, "port": PORT})

        how = self._authed()
        if how is None:
            # Still worth recording: the gate asserts the slicer never polls unauthenticated.
            with LOCK:
                REQUESTS.append({"path": path, "auth": None, "at": time.time()})
            return self._unauthorized()
        with LOCK:
            REQUESTS.append({"path": path, "auth": how, "at": time.time()})

        if path == "/api/version":
            return self._send(200, {
                "api": "2.0.0", "server": "2.1.2", "nozzle_diameter": 0.4,
                "text": "PrusaLink", "hostname": "mock-prusalink",
                "capabilities": {"upload-by-put": True},
            })

        if path == "/api/v1/status":
            with LOCK:
                printer = {
                    "state": STATE["state"],
                    "temp_nozzle": STATE["temp_nozzle"],
                    "target_nozzle": STATE["target_nozzle"],
                    "temp_bed": STATE["temp_bed"],
                    "target_bed": STATE["target_bed"],
                    "axis_z": 10.0, "flow": 100, "speed": 100,
                    "fan_hotend": 0, "fan_print": 0,
                }
            out = {"printer": printer,
                   "storage": {"path": "/usb/", "name": "usb", "read_only": False}}
            job = job_object(include_file=False)   # a real status answer carries no file name
            if job is not None:
                out["job"] = {k: v for k, v in job.items() if k != "state"}
            return self._send(200, out)

        if path == "/api/v1/job":
            job = job_object(include_file=True)
            if job is None:
                return self._send(404, {"title": "Not Found", "message": "No job"})
            return self._send(200, job)

        if path == "/api/v1/storage":
            return self._send(200, {"storage_list": [
                {"path": "/usb/", "name": "usb", "type": "USB", "read_only": False,
                 "free_space": 1234567, "available": True}]})

        return self._send(404, {"title": "Not Found", "message": path})

    # --------------------------------------------------------------------- POST --
    def do_POST(self):
        u = urlparse(self.path)
        path = u.path.rstrip("/") or "/"
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        q = parse_qs(u.query)

        def one(name, cast=str, default=None):
            v = q.get(name, [None])[0]
            if v is None or v == "":
                return default
            try:
                return cast(v)
            except (TypeError, ValueError):
                return default

        if path == "/mock/set":
            with LOCK:
                st = one("state")
                if st:
                    STATE["state"] = st.upper()
                    if STATE["state"] in MID_JOB and not STATE["job_id"]:
                        STATE["job_id"] = 34
                for key, name, cast in (("progress", "progress", float),
                                        ("time_remaining", "remaining", int),
                                        ("time_printing", "printing_s", int),
                                        ("temp_nozzle", "nozzle", float),
                                        ("target_nozzle", "target_nozzle", float),
                                        ("temp_bed", "bed", float),
                                        ("target_bed", "target_bed", float)):
                    v = one(name, cast)
                    if v is not None:
                        STATE[key] = v
                f = one("file")
                if f is not None:
                    STATE["file"] = f
                if STATE["state"] not in MID_JOB:
                    STATE["progress"] = STATE["time_remaining"] = STATE["time_printing"] = None
                    STATE["file"], STATE["job_id"] = "", 0
                return self._send(200, {"ok": True, "printer": dict(STATE)})

        if path == "/mock/clearlog":
            # The request log ONLY - what the printer is reporting is left exactly as it is. A gate
            # that counts the requests made since a preset switch needs this: /mock/reset would also
            # put the printer back to IDLE and so undo the state the gate had just set up.
            with LOCK:
                del REQUESTS[:]
                return self._send(200, {"ok": True, "printer": dict(STATE)})

        if path in ("/mock/idle", "/mock/reset"):
            with LOCK:
                STATE.update({"state": "IDLE", "progress": None, "time_remaining": None,
                              "time_printing": None, "file": "", "job_id": 0,
                              "temp_nozzle": 24.7, "target_nozzle": 0.0,
                              "temp_bed": 23.9, "target_bed": 0.0})
                if path == "/mock/reset":
                    del REQUESTS[:]
                return self._send(200, {"ok": True, "printer": dict(STATE)})

        return self._send(404, {"title": "Not Found", "message": path})


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print("mock_prusalink on 127.0.0.1:%d  apikey=%s  digest=%s/%s"
          % (PORT, APIKEY, DIGEST_USER, DIGEST_PASS), flush=True)
    srv.serve_forever()

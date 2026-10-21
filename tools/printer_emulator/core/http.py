"""HTTP plumbing shared by every personality: one listener per (address, port), a request object,
a response object, and the handler that times each request and hands it to the printer.

Lifted from tests/slic3rutils/fake_moonraker.py (reply helpers, the listener that `down` really
closes) and generalised so one printer can own several listeners (the QIDI 8080 retry port).
"""
import json
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, urlparse


class Request:
    def __init__(self, method, path, query, headers, body, port, client):
        self.method = method
        self.path = path
        self.query = query                      # raw query string, undecoded
        self.args = dict(parse_qsl(query, keep_blank_values=True))
        self.headers = headers
        self.body = body
        self.port = port
        self.client = client

    def json_body(self):
        try:
            v = json.loads(self.body or b"{}")
            return v if isinstance(v, dict) else {}
        except ValueError:
            return {}


class Response:
    """What a personality answers. `kind` and `payload` are what the command log records; `hold`
    means accept and never answer (the timeout fault); `log=False`: the entries were already
    written (sub-commands)."""

    def __init__(self, status=200, body=None, ctype="application/json", kind="http", payload=None, headers=None,
                 hold=False, log=True, stream=None, drop=False):
        self.status = status
        self.body = body
        self.ctype = ctype
        self.kind = kind
        self.payload = payload if payload is not None else {}
        self.headers = headers or {}
        self.hold = hold
        self.log = log
        # stream(wfile): an endless body (the MJPEG camera), written after the headers with the
        # printer's lock released; it returns when the client goes or the printer stops.
        self.stream = stream
        # drop: close the connection without a byte of answer (a camera turning a viewer away).
        self.drop = drop

    def encoded(self):
        if self.body is None:
            return b""
        if isinstance(self.body, (bytes, bytearray)):
            return bytes(self.body)
        if isinstance(self.body, str):
            return self.body.encode("utf-8")
        return json.dumps(self.body).encode("utf-8")


def moonraker_error(code, message, kind="http", payload=None):
    """Moonraker's error shape: {"error": {"code": 400, "message": "...", "traceback": ""}}."""
    return Response(code, {"error": {"code": code, "message": message, "traceback": ""}}, kind=kind,
                    payload=payload)


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    # Windows SO_REUSEADDR lets a second socket steal a port that is in use; never set it there.
    allow_reuse_address = sys.platform != "win32"

    def __init__(self, addr, handler, printer):
        self.printer = printer
        super().__init__(addr, handler)

    def server_bind(self):
        # HTTPServer.server_bind asks socket.getfqdn() for the address, a reverse DNS lookup that
        # takes about 5 s per 127.0.0.x listener on Windows. The name is never used here.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


class PrinterHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "EmuPrinter/1"

    def log_message(self, fmt, *a):
        pass

    def _read_body(self):
        if (self.headers.get("Transfer-Encoding") or "").lower() == "chunked":
            chunks = []
            while True:
                line = self.rfile.readline()
                size = int(line.split(b";", 1)[0].strip() or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.readline()
            return b"".join(chunks)
        n = int(self.headers.get("Content-Length") or 0)
        if self.headers.get("Expect", "").lower() == "100-continue":
            # curl sends Expect: 100-continue for big uploads; http.server answers it for us only
            # in handle_expect_100, which runs before this. Nothing else to do here.
            pass
        return self.rfile.read(n) if n else b""

    def _handle(self, method):
        t0 = time.time()
        u = urlparse(self.path)
        if (self.headers.get("Upgrade") or "").lower() == "websocket":
            # A websocket (Elegoo SDCP): the printer takes the connection over until it closes.
            req = Request(method, u.path, u.query, self.headers, b"", self.server.server_address[1], self.client_address[0])
            self.close_connection = True
            try:
                self.server.printer.handle_upgrade(self, req)
            except (OSError, EOFError, ValueError):
                pass
            return
        try:
            body = self._read_body()
        except (OSError, ValueError):
            return
        req = Request(method, u.path, u.query, self.headers, body, self.server.server_address[1],
                      self.client_address[0])
        printer = self.server.printer
        try:
            resp = printer.handle(req)
        except Exception as e:  # noqa: BLE001 - a bug in a personality must not kill the listener
            resp = Response(500, {"error": {"code": 500, "message": "emulator error: %s" % e}}, kind="error",
                            payload={"exception": repr(e)})
        if resp.hold:
            printer.record(req, resp, int((time.time() - t0) * 1000), status=None)
            printer.hold_until_released()
            self.close_connection = True
            return
        if resp.drop:
            printer.record(req, resp, int((time.time() - t0) * 1000), status=None)
            self.close_connection = True
            return
        if resp.stream is not None:
            try:
                self.send_response(resp.status)
                self.send_header("Content-Type", resp.ctype)
                self.send_header("Cache-Control", "no-cache, no-store")
                self.send_header("Connection", "close")
                for k, v in resp.headers.items():
                    self.send_header(k, v)
                self.end_headers()
                printer.record(req, resp, int((time.time() - t0) * 1000), status=resp.status)
                resp.stream(self.wfile)
            except (OSError, ValueError):
                pass
            self.close_connection = True
            return
        data = resp.encoded()
        try:
            self.send_response(resp.status)
            self.send_header("Content-Type", resp.ctype)
            self.send_header("Content-Length", str(len(data)))
            for k, v in resp.headers.items():
                self.send_header(k, v)
            self.end_headers()
            if method != "HEAD":
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        if resp.log:
            printer.record(req, resp, int((time.time() - t0) * 1000), status=resp.status)

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")

    def do_PUT(self):
        self._handle("PUT")

    def do_DELETE(self):
        self._handle("DELETE")

    def do_HEAD(self):
        self._handle("HEAD")


class Listener:
    """One listening socket for a printer, which `down` really closes (connection refused) and `up`
    opens again."""

    def __init__(self, printer, host, port):
        self.printer = printer
        self.host = host
        self.port = port
        self.server = None
        self.thread = None

    def up(self):
        if self.server:
            return
        self.server = _Server((self.host, self.port), PrinterHandler, self.printer)
        self.thread = threading.Thread(target=self.server.serve_forever, name="emu-%s-%d" % (self.host, self.port),
                                       daemon=True)
        self.thread.start()

    def down(self):
        if not self.server:
            return
        self.server.shutdown()
        self.server.server_close()
        self.server = None

    @property
    def running(self):
        return self.server is not None

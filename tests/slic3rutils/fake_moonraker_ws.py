"""A fake U1 Moonraker on 127.0.0.1 that answers both HTTP and its JSON-RPC WebSocket (/websocket)
on one port, for testing the read-only probe (Utils/U1MoonrakerProbe) without a printer.

  GET /server/info, /printer/info, /printer/objects/query   JSON answers shaped like Moonraker's
  GET /websocket (Upgrade: websocket)                       JSON-RPC: server.info, printer.info,
                                                            printer.objects.query; every request is
                                                            preceded by a notify_proc_stat_update
                                                            notification, as Moonraker sends them
  GET /fake/methods                                         {"methods": [every JSON-RPC method received]}
  GET /fake/drop                                            close every WebSocket and refuse new ones (503)
  GET /fake/up                                              accept WebSockets again

Every answer carries "via": "local" in its result. printer.objects.subscribe also starts a
notify_status_update. Any other method answers -32601 "Method not found" (an MQTT-only method).

  --no-ws   answer the WebSocket upgrade with 404 (a printer whose WebSocket is not reachable)

Nothing here talks to a real printer: it binds 127.0.0.1 only.
usage: fake_moonraker_ws.py --port P [--no-ws]   (prints "READY <port>", runs until killed)
"""
import argparse
import base64
import hashlib
import json
import socket
import socketserver
import struct
import sys
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
METHODS = []
LOCK = threading.Lock()
ARGS = None

DROPPED = {"on": False}
SOCKETS = set()

ANSWERS = {
    "server.info": {"klippy_connected": True, "klippy_state": "ready", "components": ["webcam"], "via": "local"},
    "printer.info": {"state": "ready", "state_message": "Printer is ready", "hostname": "fake-u1", "via": "local"},
    "printer.objects.query": {"eventtime": 1.0, "status": {"print_stats": {"state": "standby"},
                                                            "extruder": {"temperature": 25.0, "target": 0.0}}, "via": "local"},
    "printer.objects.subscribe": {"eventtime": 1.0, "status": {"print_stats": {"state": "standby"}}, "via": "local"},
}
HTTP = {"/server/info": "server.info", "/printer/info": "printer.info", "/printer/objects/query": "printer.objects.query"}


def http_reply(wfile, code, obj):
    body = json.dumps(obj).encode()
    reason = {200: "OK", 404: "Not Found"}.get(code, "OK")
    head = "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % (
        code, reason, len(body))
    wfile.write(head.encode() + body)


def send_frame(wfile, opcode, data):
    head = bytes([0x80 | opcode])
    n = len(data)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    wfile.write(head + data)


def read_exact(rfile, n):
    data = b""
    while len(data) < n:
        chunk = rfile.read(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def read_frame(rfile):
    b1, b2 = read_exact(rfile, 2)
    opcode = b1 & 0x0F
    n = b2 & 0x7F
    if n == 126:
        n = struct.unpack(">H", read_exact(rfile, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", read_exact(rfile, 8))[0]
    mask = read_exact(rfile, 4) if b2 & 0x80 else b"\0\0\0\0"
    data = bytearray(read_exact(rfile, n))
    for i in range(n):
        data[i] ^= mask[i % 4]
    return opcode, bytes(data)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode("latin-1").strip()
        if not line:
            return
        parts = line.split()
        path = parts[1] if len(parts) > 1 else "/"
        headers = {}
        while True:
            h = self.rfile.readline().decode("latin-1")
            if h in ("\r\n", "\n", ""):
                break
            k, _, v = h.partition(":")
            headers[k.strip().lower()] = v.strip()
        base = path.split("?")[0]
        if base == "/websocket" and headers.get("upgrade", "").lower() == "websocket":
            if ARGS.no_ws:
                return http_reply(self.wfile, 404, {"error": {"code": 404, "message": "Not Found"}})
            if DROPPED["on"]:
                return http_reply(self.wfile, 503, {"error": {"code": 503, "message": "Unavailable"}})
            accept = base64.b64encode(hashlib.sha1((headers.get("sec-websocket-key", "") + GUID).encode()).digest()).decode()
            self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
            return self.websocket()
        if base == "/fake/methods":
            with LOCK:
                return http_reply(self.wfile, 200, {"methods": list(METHODS)})
        if base == "/fake/drop":
            DROPPED["on"] = True
            with LOCK:
                socks = list(SOCKETS)
            for s in socks:
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            return http_reply(self.wfile, 200, {"dropped": len(socks)})
        if base == "/fake/up":
            DROPPED["on"] = False
            return http_reply(self.wfile, 200, {"up": True})
        if base in HTTP:
            return http_reply(self.wfile, 200, {"result": ANSWERS[HTTP[base]]})
        http_reply(self.wfile, 404, {"error": {"code": 404, "message": "Not Found"}})

    def websocket(self):
        with LOCK:
            SOCKETS.add(self.request)
        try:
            while True:
                opcode, data = read_frame(self.rfile)
                if opcode == 8:
                    send_frame(self.wfile, 8, data[:2])
                    return
                if opcode == 9:
                    send_frame(self.wfile, 10, data)
                    continue
                if opcode != 1:
                    continue
                req = json.loads(data.decode())
                method = req.get("method", "")
                with LOCK:
                    METHODS.append(method)
                note = {"jsonrpc": "2.0", "method": "notify_proc_stat_update", "params": [{"cpu_temp": 40.0}]}
                send_frame(self.wfile, 1, json.dumps(note).encode())
                if method in ANSWERS:
                    res = {"jsonrpc": "2.0", "id": req.get("id"), "result": ANSWERS[method]}
                else:
                    res = {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32601, "message": "Method not found"}}
                send_frame(self.wfile, 1, json.dumps(res).encode())
                if method == "printer.objects.subscribe":
                    update = {"jsonrpc": "2.0", "method": "notify_status_update",
                              "params": [{"print_stats": {"state": "standby"}, "via": "local"}, 2.0]}
                    send_frame(self.wfile, 1, json.dumps(update).encode())
        except (EOFError, ConnectionError, OSError, ValueError):
            return
        finally:
            with LOCK:
                SOCKETS.discard(self.request)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--no-ws", action="store_true")
    ARGS = ap.parse_args()
    srv = Server(("127.0.0.1", ARGS.port), Handler)
    print("READY %d" % ARGS.port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())

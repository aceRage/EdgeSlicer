"""A fake Snapmaker cloud broker and a fake U1 (its pairing broker and its LAN broker) on
127.0.0.1, for testing the U1 local-first route (GUI/U1LanRoute) end to end without a printer or
Snapmaker's cloud. Plain MQTT 3.1.1 only (no TLS): the test builds its LAN session without a
certificate. Nothing here ever talks to a real printer or the internet: every port binds 127.0.0.1.

Three brokers, one tiny MQTT 3.1.1 implementation (CONNECT, SUBSCRIBE, UNSUBSCRIBE, PUBLISH QoS 0/1,
PINGREQ, DISCONNECT, '+' and '#' filters; a second CONNECT with a client id already connected
drops the first, as real brokers do, and is counted as a "kick"):

  cloud    answers JSON-RPC on <sn>/request with {"result": {"via": "cloud", ...}} on <sn>/response
  lan      the same with "via": "lan"; it accepts only client ids the printer authorised
  pairing  <code>/config/request -> <code>/config/response:
           server.client_manager.confirm_lan_status {clientid}
               authorised   -> {state: success, clientid, ca, cert, key, port: <lan port>, sn}
               otherwise    -> {state: unauthorized}
           server.client_manager.request_lan_auth {clientid, app_id}
               -> {state: authorizing, clientid, app_id}; the id waits for /approve

Control (HTTP, GET):
  /authorize?cid=X   authorise X at once        /approve?cid=X   approve a waiting X
  /lan/down          stop the LAN broker (listener and sessions)   /lan/up   start it again
  /lan/state         {"up": bool}               /stats   counters, sessions, pending ids
  /quit              stop everything

usage: mock_u1_mqtt.py --cloud P1 --lan P2 --pairing P3 --control P4 [--sn SN1] [--code 12345678]
       prints "READY <cloud> <lan> <pairing> <control>" once every port listens.
"""
import argparse
import asyncio
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ARGS = None
LOOP = None
STATS = {"kicks": 0, "refused": 0, "requests": {"cloud": 0, "lan": 0},
         "pairing": {"confirm_lan_status": 0, "request_lan_auth": 0}}
AUTHORIZED = set()
PENDING = set()


def enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def packet(first, body):
    return bytes([first]) + enc_len(len(body)) + body


def mstr(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def matches(filt, topic):
    f, t = filt.split("/"), topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t):
            return False
        if part != "+" and part != t[i]:
            return False
    return len(f) == len(t)


class Session:
    def __init__(self, broker, reader, writer):
        self.broker, self.reader, self.writer = broker, reader, writer
        self.client_id = None
        self.subs = {}
        self.closed = False

    def send(self, data):
        if not self.closed:
            try:
                self.writer.write(data)
            except Exception:
                self.closed = True

    def publish(self, topic, payload):
        self.send(packet(0x30, mstr(topic) + payload))

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.writer.close()
            except Exception:
                pass

    async def read_packet(self):
        first = (await self.reader.readexactly(1))[0]
        mult, length = 1, 0
        while True:
            b = (await self.reader.readexactly(1))[0]
            length += (b & 0x7F) * mult
            if not b & 0x80:
                break
            mult *= 128
        body = await self.reader.readexactly(length) if length else b""
        return first, body

    async def run(self):
        try:
            while not self.closed:
                first, body = await self.read_packet()
                kind = first >> 4
                if kind == 1:
                    self.on_connect(body)
                elif kind == 3:
                    self.on_publish(first, body)
                elif kind == 8:
                    pid = body[0:2]
                    i, granted = 2, bytearray()
                    while i < len(body):
                        n = int.from_bytes(body[i:i + 2], "big")
                        topic = body[i + 2:i + 2 + n].decode()
                        qos = body[i + 2 + n]
                        i += 3 + n
                        self.subs[topic] = qos
                        granted.append(min(qos, 1))
                    self.send(packet(0x90, pid + bytes(granted)))
                elif kind == 10:
                    pid = body[0:2]
                    i = 2
                    while i < len(body):
                        n = int.from_bytes(body[i:i + 2], "big")
                        self.subs.pop(body[i + 2:i + 2 + n].decode(), None)
                        i += 2 + n
                    self.send(packet(0xB0, pid))
                elif kind == 12:
                    self.send(bytes([0xD0, 0]))
                elif kind == 14:
                    break
                # 4 (PUBACK) and the rest: nothing to do
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            self.close()
            self.broker.drop(self)

    def on_connect(self, body):
        n = int.from_bytes(body[0:2], "big")
        i = 2 + n + 1 + 1 + 2  # protocol name, level, flags, keepalive
        n = int.from_bytes(body[i:i + 2], "big")
        self.client_id = body[i + 2:i + 2 + n].decode()
        if not self.broker.admit(self.client_id):
            STATS["refused"] += 1
            self.send(packet(0x20, bytes([0, 5])))  # not authorised
            self.close()
            return
        old = self.broker.sessions.get(self.client_id)
        if old is not None and old is not self:
            STATS["kicks"] += 1
            old.close()
        self.broker.sessions[self.client_id] = self
        self.send(packet(0x20, bytes([0, 0])))

    def on_publish(self, first, body):
        qos = (first >> 1) & 3
        n = int.from_bytes(body[0:2], "big")
        topic = body[2:2 + n].decode()
        i = 2 + n
        if qos:
            pid = body[i:i + 2]
            i += 2
            self.send(packet(0x40, pid))
        payload = body[i:]
        self.broker.route(topic, payload)
        self.broker.on_message(topic, payload)


class Broker:
    def __init__(self, name, port, admit=None, on_message=None):
        self.name, self.port = name, port
        self.admit = admit or (lambda cid: True)
        self.on_message = on_message or (lambda t, p: None)
        self.sessions = {}
        self.all = set()
        self.server = None

    async def start(self):
        self.server = await asyncio.start_server(self.accept, "127.0.0.1", self.port)

    async def stop(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()
            self.server = None
        for s in list(self.all):
            s.close()
        self.all.clear()
        self.sessions.clear()

    async def accept(self, reader, writer):
        s = Session(self, reader, writer)
        self.all.add(s)
        await s.run()

    def drop(self, s):
        self.all.discard(s)
        if s.client_id and self.sessions.get(s.client_id) is s:
            del self.sessions[s.client_id]

    def route(self, topic, payload):
        for s in list(self.all):
            if any(matches(f, topic) for f in s.subs):
                s.publish(topic, payload)


def rpc_answer(broker, via):
    def on_message(topic, payload):
        if not topic.endswith("/request"):
            return
        sn = topic[: -len("/request")]
        try:
            req = json.loads(payload)
        except ValueError:
            return
        STATS["requests"][via] += 1
        res = {"jsonrpc": "2.0", "id": req.get("id"), "result": {"via": via, "method": req.get("method")}}
        broker.route(sn + "/response", json.dumps(res).encode())
    return on_message


def pairing_answer(broker):
    def on_message(topic, payload):
        if topic != ARGS.code + "/config/request":
            return
        try:
            req = json.loads(payload)
        except ValueError:
            return
        method = req.get("method", "")
        params = req.get("params") or {}
        cid = params.get("clientid", "")
        if method.endswith("confirm_lan_status"):
            STATS["pairing"]["confirm_lan_status"] += 1
            if cid in AUTHORIZED:
                result = {"state": "success", "clientid": cid, "ca": "CA", "cert": "CERT", "key": "KEY",
                          "port": ARGS.lan, "sn": ARGS.sn}
            else:
                result = {"state": "unauthorized", "clientid": cid}
        elif method.endswith("request_lan_auth"):
            STATS["pairing"]["request_lan_auth"] += 1
            PENDING.add(cid)
            result = {"state": "authorizing", "clientid": cid, "app_id": params.get("app_id", "")}
        else:
            return
        res = {"jsonrpc": "2.0", "id": req.get("id"), "result": result}
        broker.route(ARGS.code + "/config/response", json.dumps(res).encode())
    return on_message


BROKERS = {}


def make_brokers():
    cloud = Broker("cloud", ARGS.cloud)
    cloud.on_message = rpc_answer(cloud, "cloud")
    lan = Broker("lan", ARGS.lan, admit=lambda cid: cid in AUTHORIZED)
    lan.on_message = rpc_answer(lan, "lan")
    pairing = Broker("pairing", ARGS.pairing)
    pairing.on_message = pairing_answer(pairing)
    BROKERS.update(cloud=cloud, lan=lan, pairing=pairing)


def on_loop(coro):
    return asyncio.run_coroutine_threadsafe(coro, LOOP).result(10)


class Control(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/authorize":
            AUTHORIZED.add(q.get("cid", ""))
            return self.reply({"ok": True})
        if u.path == "/approve":
            cid = q.get("cid", "")
            if cid in PENDING:
                PENDING.discard(cid)
                AUTHORIZED.add(cid)
                return self.reply({"ok": True})
            return self.reply({"ok": False}, 404)
        if u.path == "/lan/down":
            on_loop(BROKERS["lan"].stop())
            return self.reply({"up": False})
        if u.path == "/lan/up":
            if BROKERS["lan"].server is None:
                on_loop(BROKERS["lan"].start())
            return self.reply({"up": True})
        if u.path == "/lan/state":
            return self.reply({"up": BROKERS["lan"].server is not None})
        if u.path == "/stats":
            return self.reply(dict(STATS, pending=sorted(PENDING), authorized=sorted(AUTHORIZED),
                                   sessions={n: sorted(b.sessions) for n, b in BROKERS.items()}))
        if u.path == "/quit":
            self.reply({"ok": True})
            LOOP.call_soon_threadsafe(LOOP.stop)
            return
        self.reply({"error": "unknown"}, 404)


def main():
    global ARGS, LOOP
    ap = argparse.ArgumentParser()
    ap.add_argument("--cloud", type=int, required=True)
    ap.add_argument("--lan", type=int, required=True)
    ap.add_argument("--pairing", type=int, required=True)
    ap.add_argument("--control", type=int, required=True)
    ap.add_argument("--sn", default="SN1")
    ap.add_argument("--code", default="12345678")
    ARGS = ap.parse_args()
    LOOP = asyncio.new_event_loop()
    asyncio.set_event_loop(LOOP)
    make_brokers()
    for b in BROKERS.values():
        LOOP.run_until_complete(b.start())
    httpd = ThreadingHTTPServer(("127.0.0.1", ARGS.control), Control)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    print("READY %d %d %d %d" % (ARGS.cloud, ARGS.lan, ARGS.pairing, ARGS.control), flush=True)
    try:
        LOOP.run_forever()
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())

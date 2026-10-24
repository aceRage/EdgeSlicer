"""A tiny MQTT 3.1.1 broker for one emulated printer's port (the Snapmaker U1's pairing and LAN
brokers). Plain TCP only, no TLS.

Lifted from tests/slic3rutils/mock_u1_mqtt.py (its packet codec, topic filters and session rules:
CONNECT, SUBSCRIBE, UNSUBSCRIBE, PUBLISH QoS 0/1, PINGREQ, DISCONNECT, '+' and '#'; a second
CONNECT with a client id already connected drops the first), moved from asyncio onto threads so it
sits beside the HTTP listeners. That mock's fake Snapmaker CLOUD broker is deliberately not here:
the emulator never pretends to be a cloud service (design 3.3, rail 3).
"""
import socket
import socketserver
import threading


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


class _Session(socketserver.BaseRequestHandler):
    def setup(self):
        self.broker = self.server.broker
        self.client_id = None
        self.subs = {}
        self.closed = False
        self.wlock = threading.Lock()
        self.rfile = self.request.makefile("rb")

    def send(self, data):
        if self.closed:
            return
        try:
            with self.wlock:
                self.request.sendall(data)
        except OSError:
            self.closed = True

    def publish(self, topic, payload):
        self.send(packet(0x30, mstr(topic) + payload))

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.request.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def _read(self, n):
        data = self.rfile.read(n)
        if len(data) < n:
            raise EOFError
        return data

    def read_packet(self):
        first = self._read(1)[0]
        mult, length = 1, 0
        while True:
            b = self._read(1)[0]
            length += (b & 0x7F) * mult
            if not b & 0x80:
                break
            mult *= 128
        return first, (self._read(length) if length else b"")

    def handle(self):
        self.broker.add(self)
        try:
            while not self.closed:
                first, body = self.read_packet()
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
                    self.broker.note("subscribe", {"client": self.client_id, "topics": sorted(self.subs)})
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
        except (EOFError, ConnectionError, OSError, IndexError, ValueError):
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
            self.broker.note("connect", {"client": self.client_id, "refused": True, "why": "not authorised"})
            self.send(packet(0x20, bytes([0, 5])))
            self.close()
            return
        self.broker.claim(self)
        self.send(packet(0x20, bytes([0, 0])))
        self.broker.note("connect", {"client": self.client_id})

    def on_publish(self, first, body):
        qos = (first >> 1) & 3
        n = int.from_bytes(body[0:2], "big")
        topic = body[2:2 + n].decode()
        i = 2 + n
        if qos:
            self.send(packet(0x40, body[i:i + 2]))
            i += 2
        payload = body[i:]
        self.broker.route(topic, payload)
        self.broker.on_message(self, topic, payload)


class _Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = False


class Broker:
    """One listening MQTT broker. `admit(client_id)` decides CONNECT; `on_message(session, topic,
    payload)` sees every PUBLISH after it was routed to the subscribers; `note(kind, payload)`
    logs."""

    def __init__(self, name, host, port, admit=None, on_message=None, note=None):
        self.name, self.host, self.port = name, host, port
        self.admit = admit or (lambda cid: True)
        self.on_message = on_message or (lambda s, t, p: None)
        self._note = note
        self.lock = threading.Lock()
        self.sessions = {}
        self.all = set()
        self.server = None

    def note(self, kind, payload):
        if self._note:
            self._note(self.name, kind, payload)

    def up(self):
        if self.server:
            return
        self.server = _Server((self.host, self.port), _Session)
        self.server.broker = self
        threading.Thread(target=self.server.serve_forever, name="emu-mqtt-%s-%d" % (self.host, self.port),
                         daemon=True).start()

    def down(self):
        server, self.server = self.server, None
        with self.lock:
            sessions = list(self.all)
            self.all.clear()
            self.sessions.clear()
        for s in sessions:
            s.close()
        if server:
            server.shutdown()
            server.server_close()

    @property
    def running(self):
        return self.server is not None

    def add(self, s):
        with self.lock:
            self.all.add(s)

    def claim(self, s):
        with self.lock:
            old = self.sessions.get(s.client_id)
            self.sessions[s.client_id] = s
        if old is not None and old is not s:
            self.note("kick", {"client": s.client_id})
            old.close()

    def drop(self, s):
        with self.lock:
            self.all.discard(s)
            if s.client_id and self.sessions.get(s.client_id) is s:
                del self.sessions[s.client_id]

    def route(self, topic, payload):
        with self.lock:
            targets = [s for s in self.all if any(matches(f, topic) for f in s.subs)]
        for s in targets:
            s.publish(topic, payload)

    def clients(self):
        with self.lock:
            return sorted(c for c in self.sessions if c)

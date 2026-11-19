"""Small protocol clients for the emulator's own tests (standard library only): a websocket client
for SDCP, Elegoo's chunked upload, multipart for Moonraker, and an MQTT 3.1.1 client. They speak to
loopback addresses only (asserted)."""
import base64
import hashlib
import json
import os
import socket
import struct
import time
import urllib.error
import urllib.request
import uuid as uuidlib

from emu_proc import assert_loopback


def http(host, port, path, method="GET", body=None, ctype=None, timeout=15, headers=None):
    assert_loopback(host)
    h = dict(headers or {})
    if ctype:
        h["Content-Type"] = ctype
    req = urllib.request.Request("http://%s:%d%s" % (host, port, path), data=body, method=method, headers=h)
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except OSError as e:
        return 0, str(e).encode()


def multipart(fields, file_field, filename, data, boundary="emuBoundary9"):
    out = b""
    for k, v in fields.items():
        out += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n" % (boundary, k, v)).encode()
    out += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n" % (boundary, file_field, filename)).encode()
    out += data + ("\r\n--%s--\r\n" % boundary).encode()
    return out, "multipart/form-data; boundary=" + boundary


def elegoo_upload(host, port, filename, data, chunk=1024, md5=None, uid=None):
    """ElegooLink::loopUpload's requests: one multipart POST per chunk. -> [(status, json)]."""
    md5 = md5 or hashlib.md5(data).hexdigest()
    uid = uid or str(uuidlib.uuid4())
    out = []
    for off in range(0, max(1, len(data)), chunk):
        body, ct = multipart({"Check": "1", "S-File-MD5": md5, "Offset": str(off), "Uuid": uid, "TotalSize": str(len(data))},
                             "File", filename, data[off:off + chunk])
        st, raw = http(host, port, "/uploadFile/upload", "POST", body, ct)
        try:
            out.append((st, json.loads(raw)))
        except ValueError:
            out.append((st, {"raw": raw[:200]}))
    return out


class WsClient:
    def __init__(self, host, port=3030, path="/websocket", timeout=10):
        assert_loopback(host)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ElegooSlicer\r\n\r\n"
                           % (path, host, port, key)).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                break
            head += chunk
        self.status = int(head.split(b" ", 2)[1]) if head.startswith(b"HTTP/") else 0
        self.buf = b""

    @property
    def ok(self):
        return self.status == 101

    def send(self, text):
        data = text.encode() if isinstance(text, str) else text
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _exact(self, n):
        while len(self.buf) < n:
            c = self.sock.recv(65536)
            if not c:
                raise EOFError
            self.buf += c
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, timeout=5):
        self.sock.settimeout(timeout)
        b1, b2 = self._exact(2)
        n = b2 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._exact(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._exact(8))[0]
        data = self._exact(n)
        if b1 & 0x0F == 8:
            raise EOFError
        return data.decode("utf-8", "replace")

    def recv_json(self, timeout=5, want=None):
        """The next JSON frame (that `want(frame)` accepts, skipping others, e.g. status pushes)."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            text = self.recv(max(0.1, timeout - (time.time() - t0)))
            try:
                j = json.loads(text)
            except ValueError:
                continue
            if want is None or want(j):
                return j
        raise TimeoutError("no matching frame")

    def cmd(self, cmd, data=None, rid=None):
        rid = rid or uuidlib.uuid4().hex
        self.send(json.dumps({"Id": "", "Data": {"Cmd": cmd, "Data": data or {}, "RequestID": rid, "MainboardID": "",
                                                  "TimeStamp": int(time.time() * 1000), "From": 1}}))
        return rid

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def is_response(cmd):
    return lambda j: isinstance(j.get("Data"), dict) and j["Data"].get("Cmd") == cmd


# ---- MQTT 3.1.1 ---------------------------------------------------------------------------------
def _enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _mstr(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


class MqttClient:
    def __init__(self, host, port, client_id, timeout=5, user=None, password=None):
        assert_loopback(host)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        flags = 2 | (0x80 if user is not None else 0) | (0x40 if password is not None else 0)
        body = _mstr("MQTT") + bytes([4, flags]) + (30).to_bytes(2, "big") + _mstr(client_id)
        if user is not None:
            body += _mstr(user)
        if password is not None:
            body += _mstr(password)
        self.sock.sendall(bytes([0x10]) + _enc_len(len(body)) + body)
        first, body = self.read()
        self.connack = body[1] if first >> 4 == 2 and len(body) > 1 else -1

    def _exact(self, n):
        out = b""
        while len(out) < n:
            c = self.sock.recv(n - len(out))
            if not c:
                raise EOFError
            out += c
        return out

    def read(self):
        first = self._exact(1)[0]
        mult, length = 1, 0
        while True:
            b = self._exact(1)[0]
            length += (b & 0x7F) * mult
            if not b & 0x80:
                break
            mult *= 128
        return first, (self._exact(length) if length else b"")

    def subscribe(self, topic):
        body = (1).to_bytes(2, "big") + _mstr(topic) + bytes([0])
        self.sock.sendall(bytes([0x82]) + _enc_len(len(body)) + body)
        while True:
            first, _b = self.read()
            if first >> 4 == 9:
                return

    def publish(self, topic, payload):
        body = _mstr(topic) + (payload.encode() if isinstance(payload, str) else payload)
        self.sock.sendall(bytes([0x30]) + _enc_len(len(body)) + body)

    def next_message(self, timeout=5):
        self.sock.settimeout(timeout)
        while True:
            first, body = self.read()
            if first >> 4 == 3:
                n = int.from_bytes(body[0:2], "big")
                return body[2:2 + n].decode(), body[2 + n:]

    def rpc(self, req_topic, resp_topic, method, params, rid=1):
        self.publish(req_topic, json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}))
        t0 = time.time()
        while time.time() - t0 < 5:
            topic, payload = self.next_message()
            if topic == resp_topic:
                j = json.loads(payload)
                if j.get("id") == rid:
                    return j
        raise TimeoutError(method)

    def close(self):
        try:
            self.sock.sendall(bytes([0xE0, 0]))
            self.sock.close()
        except OSError:
            pass

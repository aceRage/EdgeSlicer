"""A server-side websocket (RFC 6455) on one of a printer's HTTP listeners.

Lifted from tests/phone/mock_elegoo_sdcp.py (its handshake, frame reader and frame writer), turned
into a connection object so one printer can hold several clients and push to all of them. Text and
binary frames, ping/pong, close. No extensions, no fragmentation on send (the clients EdgeSlicer
uses never need either).
"""
import base64
import hashlib
import struct
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 1, 2, 8, 9, 10


def accept_key(key):
    return base64.b64encode(hashlib.sha1((key + GUID).encode("latin-1")).digest()).decode("ascii")


def handshake(handler):
    """Answer the upgrade on a BaseHTTPRequestHandler. False when the request is no websocket
    handshake (no key)."""
    key = handler.headers.get("Sec-WebSocket-Key", "")
    if not key:
        return False
    handler.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: %s\r\n\r\n" % accept_key(key)).encode("latin-1"))
    handler.wfile.flush()
    return True


def _read_exact(rfile, n):
    data = b""
    while len(data) < n:
        chunk = rfile.read(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


class WsConnection:
    """One websocket client. `send_text` is safe from any thread; `recv` belongs to the thread
    that owns the connection."""

    def __init__(self, handler, peer=""):
        self.rfile = handler.rfile
        self.wfile = handler.wfile
        self.sock = handler.connection
        self.peer = peer
        self.wlock = threading.Lock()
        self.closed = False

    def send(self, opcode, data):
        if self.closed:
            raise OSError("closed")
        head = bytes([0x80 | opcode])
        n = len(data)
        if n < 126:
            head += bytes([n])
        elif n < 65536:
            head += bytes([126]) + struct.pack(">H", n)
        else:
            head += bytes([127]) + struct.pack(">Q", n)
        with self.wlock:
            self.wfile.write(head + data)
            self.wfile.flush()

    def send_text(self, text):
        self.send(OP_TEXT, text.encode("utf-8") if isinstance(text, str) else text)

    def recv(self):
        """-> (opcode, bytes). Pings are answered here and not returned; a close frame is answered
        and raises EOFError."""
        while True:
            b1, b2 = _read_exact(self.rfile, 2)
            opcode = b1 & 0x0F
            n = b2 & 0x7F
            if n == 126:
                n = struct.unpack(">H", _read_exact(self.rfile, 2))[0]
            elif n == 127:
                n = struct.unpack(">Q", _read_exact(self.rfile, 8))[0]
            mask = _read_exact(self.rfile, 4) if b2 & 0x80 else b"\0\0\0\0"
            data = bytearray(_read_exact(self.rfile, n))
            for i in range(n):
                data[i] ^= mask[i % 4]
            data = bytes(data)
            if opcode == OP_CLOSE:
                try:
                    self.send(OP_CLOSE, data[:2])
                except OSError:
                    pass
                self.closed = True
                raise EOFError
            if opcode == OP_PING:
                self.send(OP_PONG, data)
                continue
            if opcode == OP_PONG:
                continue
            return opcode, data

    def close(self):
        """Drop the connection now (the printer going away, not a polite close)."""
        self.closed = True
        try:
            import socket
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

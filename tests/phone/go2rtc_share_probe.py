"""Offline go2rtc behaviour probe for the Device tab's own live view (tests/plan_own_camera_view.md).

Loopback only: no printer, no network, nothing downloaded. It starts the bundled go2rtc with a
throwaway config in a temp folder, and a tiny MJPEG server that stands in for the hub's
BambuCamRelay (the P1/A1 port-6000 relay) and counts how many times go2rtc connects to it.

It answers the questions the own-live-view design depends on:
  1. go2rtc takes an HTTP multipart MJPEG source (BambuCamRelay's output format);
  2. several viewers of one stream share ONE upstream (printer) connection;
  3. the player's WebSocket "mjpeg" mode (what player.html uses for P1/A1) delivers frames;
  4. a second PUT of the same stream name while viewers are attached makes go2rtc open a SECOND
     upstream connection for the next viewer (so the hub must reuse a registered stream, never
     re-PUT it), while PATCH does not;
  5. GET /api/streams?src=<name> tells "registered" (200) from "not registered" (404).

Usage:  python -I tests/phone/go2rtc_share_probe.py resources/tools/go2rtc/go2rtc.exe
Exit code 0 when every expectation holds. Re-run it whenever go2rtc is upgraded.
"""
import base64, http.server, json, os, socket, socketserver, struct, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.request

API_PORT, SRC_PORT = 18984, 18985
API = "http://127.0.0.1:%d" % API_PORT
SRC = "http://127.0.0.1:%d/bambu?ip=x&code=y" % SRC_PORT

# A minimal 8x8 baseline JPEG.
JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb004300080606070605080707070909080a0c140d0c0b0b0c19"
    "12130f141d1a1f1e1d1a1c1c20242e2720222c231c1c2837292c30313434341f27393d38323c2e333432ffc0000b08"
    "0008000801011100ffc4001f0000010501010101010100000000000000000102030405060708090a0bffc400b51000"
    "02010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f0"
    "2433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768"
    "696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3"
    "c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffda0008010100003f00fb"
    "d3ffd9")

upstream_connects = 0
lock = threading.Lock()


class Mjpeg(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        global upstream_connects
        with lock:
            upstream_connects += 1
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=bambuframe")
        self.end_headers()
        try:
            while True:
                self.wfile.write(b"--bambuframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(JPEG))
                self.wfile.write(JPEG + b"\r\n")
                self.wfile.flush()
                time.sleep(0.1)
        except Exception:
            pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def connects():
    with lock:
        return upstream_connects


def req(method, path):
    try:
        with urllib.request.urlopen(urllib.request.Request(API + path, method=method), timeout=5) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def put(method="PUT"):
    return req(method, "/api/streams?name=cam&src=" + urllib.request.quote(SRC, safe=""))


def http_viewer(seconds, out, tag):
    try:
        with urllib.request.urlopen(API + "/api/stream.mjpeg?src=cam", timeout=10) as r:
            t0, n = time.time(), 0
            while time.time() - t0 < seconds:
                b = r.read(4096)
                if not b:
                    break
                n += len(b)
            out[tag] = n
    except Exception as e:
        out[tag] = "ERR %s" % e


def ws_viewer(seconds, out, tag):
    """What player.html does for ?mode=mjpeg: /api/ws, then {"type":"mjpeg"}, then binary JPEGs."""
    s = socket.create_connection(("127.0.0.1", API_PORT), timeout=5)
    s.sendall(("GET /api/ws?src=cam HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % base64.b64encode(os.urandom(16)).decode()).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += s.recv(1)
    payload, mask = json.dumps({"type": "mjpeg"}).encode(), os.urandom(4)
    s.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
    t0, jpegs, buf = time.time(), 0, b""
    while time.time() - t0 < seconds:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 2:
            op, ln, off = buf[0] & 0x0F, buf[1] & 0x7F, 2
            if ln == 126:
                if len(buf) < 4:
                    break
                ln, off = struct.unpack(">H", buf[2:4])[0], 4
            elif ln == 127:
                if len(buf) < 10:
                    break
                ln, off = struct.unpack(">Q", buf[2:10])[0], 10
            if len(buf) < off + ln:
                break
            data, buf = buf[off:off + ln], buf[off + ln:]
            if op == 2 and data[:2] == b"\xff\xd8":
                jpegs += 1
    s.close()
    out[tag] = jpegs


def main():
    go2rtc = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="go2rtc_probe_")
    cfg = os.path.join(work, "go2rtc.yaml")
    with open(cfg, "w") as f:
        f.write('api:\n  listen: "127.0.0.1:%d"\nrtsp:\n  listen: ""\nwebrtc:\n  listen: ""\nsrtp:\n  listen: ""\n' % API_PORT)
    srv = Server(("127.0.0.1", SRC_PORT), Mjpeg)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    g2r = subprocess.Popen([go2rtc, "-config", cfg], stdout=open(os.path.join(work, "go2rtc.log"), "w"), stderr=subprocess.STDOUT)
    failures = []

    def expect(what, ok):
        print(("PASS " if ok else "FAIL ") + what)
        if not ok:
            failures.append(what)

    try:
        # go2rtc answers its API at once but registers the stream handlers only after its WebRTC
        # module has enumerated the interfaces (about 7 s on the owner's PC, longer under load):
        # wait until /api/stream.mjpeg exists (a missing handler and a missing stream are both
        # 404, so ask for the stream itself once it is registered).
        time.sleep(1)
        expect("unregistered stream answers 404", req("GET", "/api/streams?src=cam") == 404)
        expect("PUT of an HTTP MJPEG source is accepted", put() == 200)
        expect("registered stream answers 200", req("GET", "/api/streams?src=cam") == 200)
        t0 = time.time()
        while time.time() - t0 < 60:
            w = {}
            http_viewer(0.5, w, "warm")
            if isinstance(w.get("warm"), int):
                break
            time.sleep(1)
        print("go2rtc ready after %.1f s" % (time.time() - t0 + 1))
        time.sleep(3)  # let the warm-up viewer's upstream close again
        base0 = connects()

        out = {}
        ts = [threading.Thread(target=http_viewer, args=(4, out, "h%d" % i)) for i in range(2)]
        ts += [threading.Thread(target=ws_viewer, args=(4, out, "w%d" % i)) for i in range(2)]
        [t.start() for t in ts]
        time.sleep(2)
        print("upstream connects so far:", connects(), "(warm-up:", base0, ")")
        expect("four viewers (2 HTTP MJPEG, 2 player WebSocket mjpeg) share one upstream connection", connects() == base0 + 1)
        before = connects()
        expect("re-PUT of the same name is accepted", put() == 200)
        late = {}
        t = threading.Thread(target=http_viewer, args=(2, late, "late")); t.start(); time.sleep(1.5)
        expect("a re-PUT while viewers watch opens a SECOND upstream for the next viewer (why the hub must reuse)", connects() == before + 1)
        [x.join() for x in ts]; t.join()
        print("viewer results:", out, late)
        expect("every viewer got data", all(isinstance(v, int) and v > 0 for v in list(out.values()) + list(late.values())))
        expect("the player's WebSocket mjpeg mode delivered JPEG frames", out.get("w0", 0) > 5 and out.get("w1", 0) > 5)

        time.sleep(2)
        out2 = {}
        a = threading.Thread(target=http_viewer, args=(4, out2, "a")); a.start(); time.sleep(1.5)
        base = connects()
        expect("PATCH of the same name is accepted", put("PATCH") == 200)
        b = threading.Thread(target=http_viewer, args=(2, out2, "b")); b.start(); time.sleep(1.5)
        expect("PATCH keeps the existing stream: no new upstream connection", connects() == base)
        a.join(); b.join()
    finally:
        g2r.kill()
        srv.shutdown()
    print("RESULT", "OK" if not failures else "FAILED: %d" % len(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

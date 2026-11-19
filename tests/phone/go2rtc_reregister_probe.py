"""Offline probe: does a Stream-tab save double a camera's upstream connection? (fix/go2rtc-reregister-double-connection)

Loopback only: no printer, no network, nothing downloaded. Fake cameras are tiny HTTP multipart
MJPEG servers on 127.0.0.1 (the format BambuCamRelay serves for P1/A1, and a source go2rtc takes
as it takes rtspx://) that count how many connections go2rtc holds open to them.

Two parts; run either or both:

  go2rtc <go2rtc.exe>
      The bundled go2rtc on its own, to pin down the behaviour the hub's fix is built on:
        1. a re-PUT of a watched stream opens a SECOND upstream connection for the next viewer;
        2. GET /api/streams lists each stream's source (exactly, while idle);
        3. PATCH of an existing name does not switch a running stream to the new source;
        4. DELETE unlists a stream but its producer runs on until its last viewer leaves.
      Expectations are about go2rtc 1.9.14; re-run when go2rtc is upgraded.

  hub <EdgeSlicer.exe>
      The real hub (`--hub --datadir <fresh temp dir>`) end to end, through its own routes:
      POST /hub/state (what every Stream-tab save sends) on the control plane, and viewers on the
      hub's /api/ws tunnel in the player's `mjpeg` mode (what the phone and Stream tab play).
        5. a camera is registered and two viewers share one upstream connection;
        6. two more saves with no change keep exactly ONE upstream connection, and a third viewer
           still shares it (before the fix: a second connection);
        7. a changed source under the SAME name (the U1 /relay/h264 shape: fixed name) is swapped
           cleanly: the old upstream closes, the hub ends the old viewers' tunnels, a new viewer
           gets the new source, one upstream connection in all;
        8. a changed camera (the Stream tab's s<hash> name changes with the source) adds the new
           stream and deletes the old one: old upstream closed, new one shared;
        9. a camera taken off the list is deleted and its upstream closes.
      The hub is started and stopped by this script (POST /hub/quit, then its PID if needed).
      Its data dir is a new temp folder; %APPDATA%\\EdgeSlicer is never touched.

Usage:
  python -I tests/phone/go2rtc_reregister_probe.py go2rtc resources/tools/go2rtc/go2rtc.exe
  python -I tests/phone/go2rtc_reregister_probe.py hub C:/Dev/EdgeSlicerTest/<copy>/EdgeSlicer.exe
Exit code 0 when every expectation holds.
"""
import base64, http.server, json, os, shutil, socket, socketserver, struct, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.parse, urllib.request

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

failures = []


def expect(what, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + what + (("  [" + detail + "]") if detail else ""))
    if not ok:
        failures.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Camera:
    """A fake camera: HTTP multipart MJPEG on loopback that counts go2rtc's connections."""

    def __init__(self, tag):
        self.tag, self.lock, self.active, self.total = tag, threading.Lock(), 0, 0
        cam = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                with cam.lock:
                    cam.active += 1
                    cam.total += 1
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=bambuframe")
                    self.end_headers()
                    while True:
                        self.wfile.write(b"--bambuframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(JPEG))
                        self.wfile.write(JPEG + b"\r\n")
                        self.wfile.flush()
                        time.sleep(0.1)
                except Exception:
                    pass
                finally:
                    with cam.lock:
                        cam.active -= 1

        class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
            daemon_threads = True

        self.srv = S(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        self.url = "http://127.0.0.1:%d/bambu?ip=10.0.0.%d&code=%s" % (self.port, ord(tag[0]) % 250, tag)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def counts(self):
        with self.lock:
            return self.active, self.total

    def wait_active(self, n, limit=10.0):
        t0 = time.time()
        while time.time() - t0 < limit:
            if self.counts()[0] == n:
                return True
            time.sleep(0.1)
        return self.counts()[0] == n

    def stop(self):
        self.srv.shutdown()


class WsViewer:
    """What player.html does for ?mode=mjpeg: GET <path>/api/ws?src=<name>, then {"type":"mjpeg"},
    then binary JPEG frames. Runs until stop() or until the server closes the socket."""

    def __init__(self, port, path_query):
        self.frames, self.closed_by_server, self.error = 0, False, None
        self.stop_ev = threading.Event()
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                        % (path_query, base64.b64encode(os.urandom(16)).decode())).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            c = self.s.recv(1)
            if not c:
                raise RuntimeError("no WebSocket handshake")
            head += c
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError("WebSocket refused: %r" % head.split(b"\r\n")[0])
        payload, mask = json.dumps({"type": "mjpeg"}).encode(), os.urandom(4)
        self.s.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        self.s.settimeout(0.5)
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        buf = b""
        while not self.stop_ev.is_set():
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError as e:
                self.closed_by_server, self.error = True, str(e)
                break
            if not chunk:
                self.closed_by_server = True
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
                if op == 8:
                    self.closed_by_server = True
                elif op == 2 and data[:2] == b"\xff\xd8":
                    self.frames += 1

    def wait_frames(self, n=3, limit=20.0):
        t0 = time.time()
        while time.time() - t0 < limit and self.frames < n and not self.closed_by_server:
            time.sleep(0.1)
        return self.frames >= n

    def wait_closed(self, limit=10.0):
        self.t.join(limit)
        return self.closed_by_server

    def stop(self):
        self.stop_ev.set()
        try:
            self.s.close()
        except OSError:
            pass
        self.t.join(2)


# ------------------------------------------------------------------------------- go2rtc part ----

def part_go2rtc(go2rtc):
    print("== go2rtc alone:", go2rtc)
    work = tempfile.mkdtemp(prefix="g2r_rereg_probe_")
    api_port = free_port()
    api = "http://127.0.0.1:%d" % api_port
    cfg = os.path.join(work, "go2rtc.yaml")
    with open(cfg, "w") as f:
        f.write('api:\n  listen: "127.0.0.1:%d"\nrtsp:\n  listen: ""\nwebrtc:\n  listen: ""\nsrtp:\n  listen: ""\n' % api_port)
    a, b = Camera("A"), Camera("B")
    g = subprocess.Popen([go2rtc, "-config", cfg], stdout=open(os.path.join(work, "go2rtc.log"), "w"), stderr=subprocess.STDOUT)

    def req(method, path):
        try:
            with urllib.request.urlopen(urllib.request.Request(api + path, method=method), timeout=5) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, ""
        except Exception as e:
            return 0, str(e)

    def put(src, method="PUT"):
        return req(method, "/api/streams?name=cam&src=" + urllib.parse.quote(src, safe=""))[0]

    viewers = []
    try:
        t0 = time.time()
        while req("GET", "/api/streams")[0] != 200 and time.time() - t0 < 30:
            time.sleep(0.3)
        expect("PUT of an HTTP MJPEG source is accepted", put(a.url) == 200)
        st, body = req("GET", "/api/streams")
        lst = json.loads(body) if st == 200 else {}
        expect("GET /api/streams lists the idle stream with exactly its source",
               lst.get("cam", {}).get("producers", [{}])[0].get("url") == a.url)
        # The stream handlers come up after the WebRTC module enumerates interfaces (7-12 s).
        v = None
        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                v = WsViewer(api_port, "/api/ws?src=cam")
                if v.wait_frames(2, 5):
                    break
                v.stop()
            except Exception:
                pass
            v = None
            time.sleep(1)
        expect("a player WebSocket viewer gets frames", v is not None)
        if v is None:
            return
        viewers.append(v)
        expect("one viewer = one upstream connection", a.wait_active(1), "A active/total %s" % (a.counts(),))

        expect("re-PUT of the same name is accepted", put(a.url) == 200)
        v2 = WsViewer(api_port, "/api/ws?src=cam"); viewers.append(v2); v2.wait_frames(2)
        time.sleep(1)
        expect("1. a re-PUT while watched: the next viewer opens a SECOND upstream connection", a.counts()[0] == 2,
               "A active/total %s" % (a.counts(),))
        for x in viewers:
            x.stop()
        viewers.clear()
        a.wait_active(0)

        v = WsViewer(api_port, "/api/ws?src=cam"); viewers.append(v); v.wait_frames(2)
        expect("PATCH of an existing name is accepted", put(b.url, "PATCH") == 200)
        v2 = WsViewer(api_port, "/api/ws?src=cam"); viewers.append(v2); v2.wait_frames(2)
        time.sleep(1)
        expect("3. PATCH does not switch a running stream to the new source", a.counts()[0] == 1 and b.counts()[1] == 0,
               "A %s B %s" % (a.counts(), b.counts()))

        expect("DELETE is accepted", req("DELETE", "/api/streams?src=cam")[0] == 200)
        time.sleep(2)
        expect("4. after DELETE the watched producer runs on for its viewers", a.counts()[0] == 1 and not v.closed_by_server,
               "A %s" % (a.counts(),))
        expect("   ...and the name is gone", req("GET", "/api/streams?src=cam")[0] == 404)
        for x in viewers:
            x.stop()
        viewers.clear()
        expect("4. the old producer closes once its last viewer leaves", a.wait_active(0), "A %s" % (a.counts(),))
    finally:
        for x in viewers:
            x.stop()
        g.kill()
        g.wait()
        a.stop(); b.stop()
        shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------------------------- hub part ----

def part_hub(exe):
    print("== the hub:", exe)
    dd = tempfile.mkdtemp(prefix="g2r_rereg_hub_")
    hub_json = os.path.join(dd, "hub", "hub.json")
    env = dict(os.environ)
    env.pop("SNORCA_PHONE_ACCESS", None)
    cams = {t: Camera(t) for t in ("A", "B", "C")}
    p = subprocess.Popen([exe, "--hub", "--datadir", dd], env=env,
                         creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
    print("hub pid", p.pid, "data dir", dd)
    viewers = []
    hj = {}
    try:
        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                hj = json.load(open(hub_json, encoding="utf-8"))
                if hj.get("admin_port") and hj.get("secret") and hj.get("port") and hj.get("go2rtc_port"):
                    break
            except Exception:
                pass
            time.sleep(0.3)
        expect("the hub starts and writes hub.json with go2rtc running", bool(hj.get("go2rtc_port")), str({k: hj.get(k) for k in ("port", "admin_port", "go2rtc_port")}))
        if not hj.get("go2rtc_port"):
            return
        admin, port, secret = hj["admin_port"], hj["port"], hj["secret"]

        def post_state(hosts):
            body = json.dumps({"hosts": hosts, "active": [h["id"] for h in hosts]}).encode()
            r = urllib.request.Request("http://127.0.0.1:%d/hub/state" % admin, data=body, method="POST",
                                       headers={"Content-Type": "application/json", "X-Hub-Secret": secret})
            with urllib.request.urlopen(r, timeout=5) as resp:
                return resp.status

        def host(id_, name, cam):
            return {"id": id_, "alias": "Cam " + id_, "kind": "rtsps", "rkind": "go2rtc", "rname": name, "rsrc": cam.url}

        def viewer(name, limit=60):
            t0 = time.time()
            while time.time() - t0 < limit:
                try:
                    v = WsViewer(port, "/api/ws?src=%s&lt=%s" % (name, secret))
                    if v.wait_frames(2, 5):
                        viewers.append(v)
                        return v
                    v.stop()
                except Exception:
                    pass
                time.sleep(1)
            return None

        A, B, C = cams["A"], cams["B"], cams["C"]
        expect("POST /hub/state is accepted", post_state([host("c1", "sAAAA", A)]) == 200)
        v1 = viewer("sAAAA")  # first viewer waits out go2rtc's 7-12 s start
        v2 = viewer("sAAAA", 15)
        expect("5. a registered camera plays through the hub's /api/ws tunnel", v1 is not None and v2 is not None)
        if v1 is None or v2 is None:
            return
        expect("5. two viewers share one upstream connection", A.wait_active(1) and A.counts()[1] == 1, "A active/total %s" % (A.counts(),))

        for _ in range(2):  # the owner's hand test: save the Stream tab twice
            post_state([host("c1", "sAAAA", A)])
            time.sleep(2)
        v3 = viewer("sAAAA", 15)
        time.sleep(2)
        expect("6. two saves with no change keep exactly ONE upstream connection", A.counts() == (1, 1), "A active/total %s" % (A.counts(),))
        expect("6. a viewer after the saves still shares it", v3 is not None and v3.frames > 0)
        expect("6. the viewers already watching never noticed", not v1.closed_by_server and not v2.closed_by_server)

        # 7. Same name, new source (the U1 shape: u1_<id> stays, its /relay/h264 source would change).
        post_state([host("c1", "sAAAA", B)])
        expect("7. the hub ends the old stream's viewer tunnels", v1.wait_closed(10) and v2.wait_closed(5) and v3.wait_closed(5))
        expect("7. the old upstream connection closes", A.wait_active(0), "A active/total %s" % (A.counts(),))
        v4 = viewer("sAAAA", 20)
        v5 = viewer("sAAAA", 15)
        time.sleep(1)
        expect("7. new viewers get the new source, sharing one upstream connection",
               v4 is not None and v5 is not None and B.counts() == (1, 1) and A.counts()[0] == 0,
               "A %s B %s" % (A.counts(), B.counts()))

        # 8. A changed camera on the Stream tab: its s<hash> name changes with the source.
        post_state([host("c1", "sCCCC", C)])
        expect("8. the old name's viewers are ended", v4.wait_closed(10) and v5.wait_closed(5))
        expect("8. the old upstream connection closes", B.wait_active(0), "B active/total %s" % (B.counts(),))
        v6 = viewer("sCCCC", 20)
        v7 = viewer("sCCCC", 15)
        time.sleep(1)
        expect("8. the new name plays, one upstream connection", v6 is not None and v7 is not None and C.counts() == (1, 1),
               "C %s" % (C.counts(),))
        try:
            gone = WsViewer(port, "/api/ws?src=sAAAA&lt=%s" % secret)
            viewers.append(gone)
            got = gone.wait_frames(1, 4)
        except Exception:
            got = False
        expect("8. the old name is no longer registered", not got and A.counts()[0] == 0 and B.counts()[0] == 0)

        # A no-change save once more, with a camera playing: still one connection.
        post_state([host("c1", "sCCCC", C)])
        time.sleep(3)
        expect("6b. another unchanged save leaves the playing camera alone",
               C.counts() == (1, 1) and not v6.closed_by_server and not v7.closed_by_server, "C %s" % (C.counts(),))

        # 9. The camera is removed from the Stream tab.
        post_state([])
        expect("9. a removed camera's viewers are ended", v6.wait_closed(10) and v7.wait_closed(5))
        expect("9. a removed camera's upstream connection closes", C.wait_active(0), "C active/total %s" % (C.counts(),))
    finally:
        for v in viewers:
            v.stop()
        try:
            if hj.get("admin_port"):
                r = urllib.request.Request("http://127.0.0.1:%d/hub/quit" % hj["admin_port"], data=b"via=test", method="POST",
                                           headers={"X-Hub-Secret": hj.get("secret", ""), "Content-Type": "application/x-www-form-urlencoded"})
                urllib.request.urlopen(r, timeout=5).read()
        except Exception as e:
            print("quit:", e)
        try:
            p.wait(15)
        except subprocess.TimeoutExpired:
            p.kill()  # only the process this script started, by its handle
            p.wait()
        for c in cams.values():
            c.stop()
        log_dir = os.path.join(dd, "log")
        if os.path.isdir(log_dir):
            for f in sorted(os.listdir(log_dir)):
                if "hub" in f.lower():
                    for line in open(os.path.join(log_dir, f), encoding="utf-8", errors="replace"):
                        if "camera streams" in line or "go2rtc" in line.lower() and ("refused" in line or "did not" in line):
                            print("  hub.log:", line.rstrip()[-220:])
        shutil.rmtree(dd, ignore_errors=True)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("go2rtc", "hub"):
        print(__doc__)
        return 2
    args = sys.argv[1:]
    while len(args) >= 2:
        kind, path = args[0], os.path.abspath(args[1])
        args = args[2:]
        (part_go2rtc if kind == "go2rtc" else part_hub)(path)
    print("RESULT", "OK" if not failures else "FAILED: %d" % len(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

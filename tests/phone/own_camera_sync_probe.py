"""Offline probe: the Device tab's live view (POST /hub/camera) and the Stream-tab sync (#450).

Loopback only: no printer, no network, nothing downloaded, and no camera is ever connected to -
nothing here opens a viewer, and go2rtc only dials a source while somebody watches. The source is
an rtspx:// URL on 127.0.0.1, which nothing listens on.

It runs the real hub (`<EdgeSlicer.exe> --hub --datadir <fresh temp dir>`) and checks, through
the hub's own routes and go2rtc's API (credentials read from the temp data dir's go2rtc.yaml):
  1. /hub/camera registers the Device tab's s<hash> stream;
  2. asking again reports it reused (hub.log "already registered, reused");
  3. a Stream-tab save that lists the same camera (same rname/rsrc) leaves it alone;
  4. a Stream-tab save that DROPS that camera does not delete it (before the merge with #448's
     registration in the sync, #450 alone deleted it);
  5. a Stream-tab-only camera dropped by a save IS still deleted (the #450 behaviour is intact).

Usage:  python -I tests/phone/own_camera_sync_probe.py C:/Dev/EdgeSlicerTest/<copy>/EdgeSlicer.exe
Exit code 0 when every expectation holds. %APPDATA%\\EdgeSlicer is never touched.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time, urllib.error, urllib.request, base64

failures = []


def expect(what, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + what + (("  [" + detail + "]") if detail else ""))
    if not ok:
        failures.append(what)


def js_hash(s):
    h = 0x811c9dc5
    for ch in s.encode():
        h ^= ch
        h = (h * 0x01000193) & 0xFFFFFFFF
    return format(h, "x")


def main():
    exe = os.path.abspath(sys.argv[1])
    dd = tempfile.mkdtemp(prefix="own_cam_sync_")
    hub_json = os.path.join(dd, "hub", "hub.json")
    env = dict(os.environ)
    env.pop("SNORCA_PHONE_ACCESS", None)
    p = subprocess.Popen([exe, "--hub", "--datadir", dd], env=env,
                         creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
    print("hub pid", p.pid, "data dir", dd)
    hj = {}
    try:
        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                hj = json.load(open(hub_json, encoding="utf-8"))
                if hj.get("admin_port") and hj.get("secret") and hj.get("go2rtc_port"):
                    break
            except Exception:
                pass
            time.sleep(0.3)
        expect("the hub starts with go2rtc", bool(hj.get("go2rtc_port")))
        if not hj.get("go2rtc_port"):
            return
        admin, secret = hj["admin_port"], hj["secret"]
        cfg = open(os.path.join(dd, "hub", "go2rtc.yaml"), encoding="utf-8").read()
        user = re.search(r'username: "([^"]+)"', cfg).group(1)
        pw = re.search(r'password: "([^"]+)"', cfg).group(1)
        g2r_port = int(re.search(r'listen: "127\.0\.0\.1:(\d+)"', cfg).group(1))
        auth = "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode()

        def g2r_has(name):
            r = urllib.request.Request("http://127.0.0.1:%d/api/streams?src=%s" % (g2r_port, name), headers={"Authorization": auth})
            try:
                with urllib.request.urlopen(r, timeout=5) as resp:
                    return resp.status == 200
            except urllib.error.HTTPError:
                return False

        def hub_post(path, obj):
            r = urllib.request.Request("http://127.0.0.1:%d%s" % (admin, path), data=json.dumps(obj).encode(), method="POST",
                                       headers={"Content-Type": "application/json", "X-Hub-Secret": secret})
            try:
                with urllib.request.urlopen(r, timeout=30) as resp:
                    return resp.status, json.loads(resp.read() or b"{}")
            except urllib.error.HTTPError as e:
                return e.code, {}

        def settle(cond, limit=25):
            t0 = time.time()
            while time.time() - t0 < limit:
                if cond():
                    return True
                time.sleep(0.5)
            return cond()

        code = "12345678"
        src = "rtspx://bblp:%s@127.0.0.1:322/streaming/live/1" % code
        name = "s" + js_hash(src)

        st, j = hub_post("/hub/camera", {"kind": "rtsps", "ip": "127.0.0.1", "code": code})
        expect("1. /hub/camera registers the s<hash> stream", st == 200 and j.get("name") == name and settle(lambda: g2r_has(name)),
               "%s %s" % (st, j))
        st, j = hub_post("/hub/camera", {"kind": "rtsps", "ip": "127.0.0.1", "code": code})
        expect("2. asking again reuses it", st == 200 and j.get("reused") is True, "%s %s" % (st, j))

        cam = {"id": "devcam", "alias": "Same printer", "kind": "rtsps", "rkind": "go2rtc", "rname": name, "rsrc": src}
        other_src = "rtspx://bblp:87654321@127.0.0.2:322/streaming/live/1"
        other = {"id": "tabonly", "alias": "Tab only", "kind": "rtsps", "rkind": "go2rtc", "rname": "s" + js_hash(other_src), "rsrc": other_src}
        hub_post("/hub/state", {"hosts": [cam, other], "active": ["devcam", "tabonly"]})
        expect("3. a Stream-tab save listing the same camera keeps it", settle(lambda: g2r_has(name) and g2r_has(other["rname"])))

        hub_post("/hub/state", {"hosts": [], "active": []})
        time.sleep(4)  # let the sync worker run
        expect("4. a Stream-tab save that drops it does NOT delete the Device tab's stream", g2r_has(name))
        expect("5. a Stream-tab-only camera dropped by the save is deleted", settle(lambda: not g2r_has(other["rname"])))

        log = ""
        for f in os.listdir(os.path.join(dd, "log")) if os.path.isdir(os.path.join(dd, "log")) else []:
            if f.startswith("hub"):
                log += open(os.path.join(dd, "log", f), encoding="utf-8", errors="replace").read()
        expect("hub.log names the reuse", "already registered, reused" in log)
        expect("hub.log never shows the access code", code not in log)
    finally:
        try:
            if hj.get("admin_port"):
                r = urllib.request.Request("http://127.0.0.1:%d/hub/quit" % hj["admin_port"], data=b"via=test", method="POST",
                                           headers={"X-Hub-Secret": hj.get("secret", "")})
                urllib.request.urlopen(r, timeout=5).read()
        except Exception:
            pass
        try:
            p.wait(timeout=15)
        except Exception:
            p.kill()
        shutil.rmtree(dd, ignore_errors=True)
    print("RESULT", "OK" if not failures else "FAILED: %d" % len(failures))


if __name__ == "__main__":
    main()
    sys.exit(0 if not failures else 1)

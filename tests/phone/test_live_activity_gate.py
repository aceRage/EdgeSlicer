"""Gate for hub-driven Live Activity updates (src/slic3r/GUI/LiveActivityPush.hpp), end to end
against mock_apns.py. Nothing here contacts Apple, a real hub, a printer or the push service.

What it runs:
  * a hub of its own from an install tree, on a scratch data dir seeded with one printing Bambu row
    in hub/printers.json (the cache the hub serves with no slicer window open), with
    SNORCA_DEBUG_ROUTES=1 so APNs can point at the mock;
  * the mock APNs from snorca_hubtest (cleartext HTTP/2, records every request byte for byte).

What it checks:
  L1  /push/device answers the "live_activity" feature; /summary carries an opaque push_id that is
      not the serial
  L2  /push/activity: 409 before the phone opted in, 200 after; a bad token 400; the hub page
      shows the opt-in and the activity count, never a token
  L3  within a poll or two the hub sends an ActivityKit update to the ACTIVITY's token:
      apns-push-type liveactivity, topic <bundle>.push-type.liveactivity, priority 5, no collapse
      id, aps {event update, timestamp, content-state, stale-date = timestamp + 300,
      relevance-score}; content-state holds only the app's ProgressContent keys
  L4  the app forgets the activity: after the 30 s grace the hub starts one by push-to-start, to
      the START token: event start, attributes {printerId = push_id, printerName "", jobKey ""}
  L5  privacy: the serial, the printer's name, its address and the file name appear nowhere in
      any Live Activity request the mock received - headers, HPACK and body
  L6  opting out stops it: re-registering without live_activity, no more Live Activity pushes

usage: test_live_activity_gate.py --install=<install dir> --datadir=<scratch dir, wiped>
                                  [--mocks=<dir with mock_apns.py>] [--seed=<a data dir with a
                                  finished first run, copied>]
"""
import base64
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
INSTALL = opts.get("--install")
DD = opts.get("--datadir")
MOCKS = opts.get("--mocks", os.path.join(os.environ.get("TEMP", ""), "snorca_hubtest"))
SEED = opts.get("--seed", os.path.join(MOCKS, "dd_lan"))
if not INSTALL or not DD:
    raise SystemExit(__doc__)
EXE = os.path.join(INSTALL, "EdgeSlicer.exe")
HUB_JSON = os.path.join(DD, "hub", "hub.json")

SERIAL = "01P00A451299999"
NAME = "Gate X1C in the workshop"
JOB = "gate_secret_widget_v7.gcode.3mf"
IP = "192.168.77.123"
DEVICE_TOKEN = "e" * 64
ACTIVITY_TOKEN = "c1" * 80
START_TOKEN = "d2" * 80
STATE_KEYS = {"state", "percent", "layer", "totalLayers", "eta", "barStart", "stale", "asOf"}

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)


def http(url, method="GET", headers=None, body=None, timeout=20):
    req = urllib.request.Request(url, method=method, data=body)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:  # noqa
        return 0, str(e).encode()


def b64u(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def device_keys():
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    key = ec.generate_private_key(ec.SECP256R1())
    pub = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    return b64u(pub), b64u(os.urandom(16))


def hub_file():
    with open(HUB_JSON, encoding="utf-8") as f:
        return json.load(f)


def wait_for(pred, timeout, step=1.0):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return pred()


# ------------------------------------------------------------------ setup ----

if not os.path.isfile(EXE):
    raise SystemExit("no install at %s" % INSTALL)
if not os.path.isfile(os.path.join(MOCKS, "mock_apns.py")):
    raise SystemExit("no mock_apns.py in %s" % MOCKS)

mock = subprocess.Popen([sys.executable, os.path.join(MOCKS, "mock_apns.py")], stdout=subprocess.PIPE, text=True)
info = {}
for _ in range(6):
    k, _, v = mock.stdout.readline().strip().partition(" ")
    info[k] = v
MOCK = "http://127.0.0.1:%s" % info["MOCK_APNS_PORT"]
CTL = "http://127.0.0.1:%s" % info["MOCK_APNS_CTL"]
BUNDLE = info["MOCK_APNS_BUNDLE"]


def mock_requests():
    st, b = http(CTL + "/ctl/requests")
    return json.loads(b).get("requests", []) if st == 200 else []


def la_requests():
    return [r for r in mock_requests() if r["headers"].get("apns-push-type") == "liveactivity"]


shutil.rmtree(DD, ignore_errors=True)
if os.path.isdir(SEED):
    shutil.copytree(SEED, DD)
    shutil.rmtree(os.path.join(DD, "hub"), ignore_errors=True)
os.makedirs(os.path.join(DD, "hub"), exist_ok=True)
now_ms = int(time.time() * 1000)
row = {"id": SERIAL, "name": NAME, "model": "BL-P001", "kind": "bambu", "print_status": "RUNNING",
       "status": "RUNNING", "online": True, "printing": True, "percent": 42, "left_time_s": 3600,
       "layer": 120, "total_layers": 300, "task": JOB, "ip": IP, "print_error": None}
with open(os.path.join(DD, "hub", "printers.json"), "w", encoding="utf-8") as f:
    json.dump({"printers": [{"id": SERIAL, "row": row, "at": now_ms, "instance": 0}]}, f)

env = dict(os.environ, SNORCA_DEBUG_ROUTES="1")
hub_proc = subprocess.Popen([EXE, "--hub", "--datadir", DD, "--hub-phone"], env=env,
                            creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP, close_fds=True)
hj = wait_for(lambda: (lambda j: j if j.get("admin_port") else None)(
    json.load(open(HUB_JSON, encoding="utf-8")) if os.path.isfile(HUB_JSON) else {}), 120, 0.5)
if not hj:
    mock.kill()
    raise SystemExit("the hub did not come up")
LOOP = "http://127.0.0.1:%d" % hj["port"]
ADM = "http://127.0.0.1:%d" % hj["admin_port"]
R = LOOP + "/r/" + hj["token"]
H = {"X-Hub-Secret": hj["secret"], "Content-Type": "application/json"}
J = {"Content-Type": "application/json"}
print("hub pid %s on %d (admin %d), mock APNs %s" % (hj["pid"], hj["port"], hj["admin_port"], MOCK))

p256dh, auth = device_keys()
reg = {"platform": "apns", "env": "production", "token": DEVICE_TOKEN, "bundle": BUNDLE, "p256dh": p256dh,
       "auth": auth, "label": "Gate iPhone", "app": "0.3.0", "os": "iOS 26.1"}
activity = {"platform": "apns", "token": DEVICE_TOKEN, "printer": SERIAL,
            "job": "", "activity_token": ACTIVITY_TOKEN, "started_at": int(time.time())}

try:
    st, b = http(ADM + "/hub/apppush/options", "POST", H, json.dumps({
        "mode": "own", "enabled": True,
        "apns": {"enabled": True, "key_path": info["MOCK_APNS_KEY"], "key_id": info["MOCK_APNS_KEY_ID"],
                 "team_id": info["MOCK_APNS_TEAM_ID"], "bundle": BUNDLE, "env": "production", "host_override": MOCK}}).encode())
    check(st == 200, "own-key APNs pointed at the mock (%s)" % st)

    print("L1 the feature and the opaque id")
    st, b = http(R + "/push/device", "POST", J, json.dumps(reg).encode())
    feats = json.loads(b).get("features", []) if st == 200 else []
    check(st == 200 and "live_activity" in feats, "/push/device answers the live_activity feature (%s %s)" % (st, feats))
    st, b = http(R + "/summary")
    printers = json.loads(b).get("printers", []) if st == 200 else []
    mine = next((p for p in printers if p.get("id") == SERIAL), {})
    push_id = mine.get("push_id", "")
    check(bool(push_id) and SERIAL not in push_id and push_id.startswith("t"),
          "/summary carries an opaque push_id for the printer (%s)" % push_id)

    print("L2 registering an activity")
    st, b = http(R + "/push/activity", "POST", J, json.dumps(activity).encode())
    check(st == 409, "before the phone opted in: 409 (%s)" % st)
    reg_on = dict(reg, live_activity={"enabled": True, "start_token": START_TOKEN, "frequent": False})
    st, b = http(R + "/push/device", "POST", J, json.dumps(reg_on).encode())
    check(st == 200, "opting in through /push/device (%s)" % st)
    st, b = http(R + "/push/activity", "POST", J, json.dumps(dict(activity, activity_token="zz")).encode())
    check(st == 400, "a malformed activity token: 400 (%s)" % st)
    st, b = http(R + "/push/activity", "POST", {"Content-Type": "text/plain"}, json.dumps(activity).encode())
    check(st == 415, "not JSON: 415 (%s)" % st)
    st, b = http(R + "/push/activity", "POST", J, json.dumps(activity).encode())
    check(st == 200, "the activity's token: 200 (%s)" % st)
    st, b = http(ADM + "/hub/apppush", "GET", H)
    page = b.decode("utf-8", "replace")
    dev = next((d for d in json.loads(page).get("devices", []) if d.get("label") == "Gate iPhone"), {}) if st == 200 else {}
    la = dev.get("live_activity", {})
    check(la.get("enabled") is True and la.get("activities") == 1 and la.get("start_token") is True,
          "the hub page shows the opt-in and one activity (%s)" % la)
    check(ACTIVITY_TOKEN not in page and START_TOKEN not in page, "and never a token")

    print("L3 the hub pushes to the activity after a poll")
    upd = wait_for(lambda: next((r for r in la_requests() if r["device_token"] == ACTIVITY_TOKEN), None), 45)
    check(upd is not None, "an ActivityKit push reached the activity's token")
    if upd:
        h, p = upd["headers"], upd["payload"] or {}
        aps = p.get("aps", {})
        check(h.get("apns-topic") == BUNDLE + ".push-type.liveactivity", "topic %s" % h.get("apns-topic"))
        check(h.get("apns-priority") == "5", "routine update at priority 5 (%s)" % h.get("apns-priority"))
        check("apns-collapse-id" not in h or not h.get("apns-collapse-id"), "no collapse id")
        check(list(p.keys()) == ["aps"], "the body is {aps} only (%s)" % list(p.keys()))
        check(aps.get("event") == "update", "event update (%s)" % aps.get("event"))
        check(aps.get("stale-date", 0) - aps.get("timestamp", 0) == 300, "stale-date five minutes on")
        cs = aps.get("content-state", {})
        check(set(cs.keys()) <= STATE_KEYS and cs.get("state") == "printing" and cs.get("percent") == 42,
              "content-state is the app's ProgressContent (%s)" % sorted(cs.keys()))
        check(isinstance(aps.get("relevance-score"), (int, float)), "relevance-score set")

    print("L4 push-to-start when the phone has no activity for a print")
    st, b = http(R + "/push/activity", "DELETE", J, json.dumps({"platform": "apns", "token": DEVICE_TOKEN, "printer": SERIAL}).encode())
    check(st == 200, "the app says it ended the activity (%s)" % st)
    start = wait_for(lambda: next((r for r in la_requests() if r["device_token"] == START_TOKEN), None), 60)
    check(start is not None, "a push-to-start reached the start token")
    if start:
        aps = (start["payload"] or {}).get("aps", {})
        check(aps.get("event") == "start" and start["headers"].get("apns-priority") == "10", "event start at priority 10")
        check(aps.get("attributes-type") == "PrintActivityAttributes", "attributes-type is the app's struct")
        check(aps.get("attributes") == {"printerId": push_id, "printerName": "", "jobKey": ""},
              "attributes name the printer by push_id only (%s)" % aps.get("attributes"))

    print("L5 nothing identifying in any Live Activity request")
    leaks = []
    for r in la_requests():
        raw = base64.b64decode(r["raw_b64"]).decode("latin-1") + r["body"] + json.dumps(r["headers"])
        for secret in (SERIAL, NAME, "workshop", JOB, "gate_secret", IP):
            if secret in raw:
                leaks.append(secret)
    check(len(la_requests()) >= 2 and not leaks, "%d Live Activity requests, leaks: %s" % (len(la_requests()), leaks))

    print("L6 opting out stops it")
    st, b = http(R + "/push/activity", "POST", J, json.dumps(activity).encode())
    st, b = http(R + "/push/device", "POST", J, json.dumps(reg).encode())
    check(st == 200, "re-registering without live_activity (%s)" % st)
    time.sleep(3)  # a push queued just before the opt-out may still land
    before = len(la_requests())
    time.sleep(25)
    check(len(la_requests()) == before, "no Live Activity push after opting out (%d -> %d)" % (before, len(la_requests())))
    st, b = http(R + "/push/activity", "POST", J, json.dumps(activity).encode())
    check(st == 409, "and /push/activity is refused again (%s)" % st)

    st, b = http(ADM + "/hub/apppush/debug", "POST", H, json.dumps({"op": "live_activity", "row": row}).encode())
    d = json.loads(b) if st == 200 else {}
    check(st == 200 and d.get("start", {}).get("attributes", {}).get("printerId") == push_id,
          "the debug op shows the same payloads (%s)" % st)
finally:
    try:
        http(ADM + "/hub/quit", "POST", {"X-Hub-Secret": hj["secret"]}, b"via=test", timeout=8)
    except Exception:  # noqa
        pass
    for _ in range(40):
        if hub_proc.poll() is not None:
            break
        time.sleep(0.5)
    mock.kill()

print("\nLIVE ACTIVITY GATE: %s (%d failed)" % ("PASS" if not fails else "FAIL", len(fails)))
for f in fails:
    print("  - " + f)
sys.exit(1 if fails else 0)

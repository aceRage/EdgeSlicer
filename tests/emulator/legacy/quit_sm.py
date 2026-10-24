"""Quit the dd_sm slicer instance and its hub (only processes this test started).

Each instance is asked to close through its own API (discard: the test project is never edited),
then the hub is asked to quit with the per-run secret. Nothing else on the PC is touched.
"""
import json, os, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DATADIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "dd_sm")
TOKEN = "testtoken12345"
try:
    hub = json.load(open(os.path.join(DATADIR, "hub", "hub.json"), encoding="utf-8"))
except Exception as e:
    print("no hub.json:", e)
    raise SystemExit(0)
PORT, ADMIN, SECRET = hub["port"], hub.get("admin_port") or hub["port"], hub.get("secret", "")


def post(url, headers=None):
    req = urllib.request.Request(url, method="POST", data=b"via=test",
                                 headers=dict({"Content-Type": "application/x-www-form-urlencoded"}, **(headers or {})))
    try:
        return urllib.request.urlopen(req, timeout=10).status
    except Exception as e:
        return "err %s" % e


try:
    j = json.load(urllib.request.urlopen(urllib.request.Request(
        "http://127.0.0.1:%d/hub/instances" % ADMIN, headers={"X-Hub-Secret": SECRET}), timeout=5))
    for i in j.get("instances", []):
        print("quit instance", i["pid"], "->",
              post("http://127.0.0.1:%d/r/%s/i/%d/api/quit?discard=1" % (PORT, TOKEN, i["pid"])))
except Exception as e:
    print("instances:", e)
time.sleep(3)
print("quit hub ->", post("http://127.0.0.1:%d/hub/quit" % ADMIN, {"X-Hub-Secret": SECRET}))

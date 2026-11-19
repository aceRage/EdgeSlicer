"""Wait until the dd_sm hub is up and its slicer instance answers with a project title."""
import json, os, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DATADIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "dd_sm")
TOKEN = "testtoken12345"
deadline = time.time() + 600  # the machine often runs several builds at once
hub = None
while time.time() < deadline:
    try:
        hub = json.load(open(os.path.join(DATADIR, "hub", "hub.json"), encoding="utf-8"))
        insts = os.listdir(os.path.join(DATADIR, "hub", "instances"))
        for f in insts:
            pid = int(f.split(".")[0])
            j = json.load(urllib.request.urlopen(
                "http://127.0.0.1:%d/r/%s/i/%d/api/info" % (hub["port"], TOKEN, pid), timeout=5))
            if j.get("title"):
                print("ready: hub", hub["port"], "instance", pid, j["title"])
                raise SystemExit(0)
    except SystemExit:
        raise
    except Exception:
        pass
    time.sleep(2)
print("timed out waiting for the dd_sm instance")
raise SystemExit(1)

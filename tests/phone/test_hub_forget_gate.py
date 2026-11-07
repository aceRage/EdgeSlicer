"""Gate for the hub's leftover-printer rules (fix/hub-forget-printers): the prune of remembered rows
the slicer no longer has, and POST /printers/<id>/forget on both planes. Nothing here contacts a
printer, the live hub or its data dir.

What it runs:
  * a hub of its own from an install tree, on a scratch data dir whose hub/printers.json is seeded
    with remembered rows and the `configured` list a slicer window gave last time. No slicer
    window is started, so the only prune is the one load_printers applies.

What it checks:
  F1  load: a row that was on the configured list stays (offline or not); a row that was not is
      gone at once and printers.json is rewritten without it; a cloud-bound Bambu row the last
      window could not judge (configured_unsure_kinds bambu_cloud) stays
  F2  /summary carries configured true/false and /pair lists the forget_printer feature
  F3  phone plane POST /r/<token>/printers/<id>/forget (the app's Remove): 415 without a JSON
      body type, 404 for an unknown id, 409 bambu_cloud for a cloud-bound Bambu row (it stays);
      with no slicer window open the hub removes a configured printer at its source itself: a saved
      print-host device leaves print_host_devices.json, a LAN Snapmaker leaves snapmaker_lan.json
      and gets a tombstone in removed_printers.json, and the rows leave /summary and printers.json.
      /summary rows carry remove {mode, kind}.
  F4  loopback control plane: GET /hub/printers and POST /hub/printers/<id>/forget need the
      X-Hub-Secret (403 without), then answer like the phone plane
  F5  a hub restart keeps what was forgotten forgotten

usage: test_hub_forget_gate.py --install=<install dir> --datadir=<scratch dir, wiped>
                               [--seed=<a data dir with a finished first run, copied>]
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
INSTALL = opts.get("--install")
DD = opts.get("--datadir")
SEED = opts.get("--seed", "")
if not INSTALL or not DD:
    raise SystemExit(__doc__)
EXE = os.path.join(INSTALL, "EdgeSlicer.exe")
HUB_JSON = os.path.join(DD, "hub", "hub.json")
PRINTERS_JSON = os.path.join(DD, "hub", "printers.json")

CONFIGURED = "sm:GATE-CONFIGURED"      # set up in the slicer, offline
LEFTOVER = "sm:GATE-TEST-LEFTOVER"     # a test printer nobody has any more
CLOUD_A = "01P00AGATE00001"            # cloud-bound Bambu, no sign-in last time: kept, forgettable
CLOUD_B = "01P00AGATE00002"            # the same, forgotten through the control plane
PH_DEV = "ph:gatedev01"                # a saved print-host device (a Qidi "Test")
SM_DEV = "sm:GATE-U1-SN"               # a LAN Snapmaker found by discovery

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


def wait_for(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return pred()


def saved_ids():
    try:
        with open(PRINTERS_JSON, encoding="utf-8") as f:
            return {e.get("id") for e in json.load(f).get("printers", [])}
    except Exception:  # noqa
        return set()


def start_hub():
    try:
        os.remove(HUB_JSON)
    except OSError:
        pass
    proc = subprocess.Popen([EXE, "--hub", "--datadir", DD, "--hub-phone"],
                            creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP, close_fds=True)
    hj = wait_for(lambda: (lambda j: j if j.get("admin_port") else None)(
        json.load(open(HUB_JSON, encoding="utf-8")) if os.path.isfile(HUB_JSON) else {}), 120)
    if not hj:
        proc.kill()
        raise SystemExit("the hub did not come up")
    return proc, hj


def stop_hub(proc, hj):
    try:
        http("http://127.0.0.1:%d/hub/quit" % hj["admin_port"], "POST", {"X-Hub-Secret": hj["secret"]}, b"via=test", timeout=8)
    except Exception:  # noqa
        pass
    for _ in range(40):
        if proc.poll() is not None:
            return
        time.sleep(0.5)
    proc.kill()  # our own process, by handle


def summary_rows(R):
    st, b = http(R + "/summary")
    return {p["id"]: p for p in json.loads(b).get("printers", [])} if st == 200 else {}


# ------------------------------------------------------------------ setup ----

if not os.path.isfile(EXE):
    raise SystemExit("no install at %s" % INSTALL)
shutil.rmtree(DD, ignore_errors=True)
if SEED and os.path.isdir(SEED):
    shutil.copytree(SEED, DD)
    shutil.rmtree(os.path.join(DD, "hub"), ignore_errors=True)
os.makedirs(os.path.join(DD, "hub"), exist_ok=True)
now_ms = int(time.time() * 1000)
day_ago = now_ms - 24 * 3600 * 1000


def row(pid, kind, name, **extra):
    r = {"id": pid, "kind": kind, "name": name, "online": False, "status": "offline"}
    r.update(extra)
    return {"id": pid, "row": r, "at": day_ago, "instance": 0}


with open(PRINTERS_JSON, "w", encoding="utf-8") as f:
    json.dump({"printers": [row(CONFIGURED, "snapmaker", "Gate U1 (off)"),
                            row(LEFTOVER, "snapmaker", "Gate mock U1"),
                            row(CLOUD_A, "bambu", "Gate cloud A", lan_mode=False),
                            row(CLOUD_B, "bambu", "Gate cloud B", lan_mode=False),
                            row(PH_DEV, "printhost", "Test", model_key="Gate Qidi X-Max 4", device_id="gatedev01", ip="127.0.0.32"),
                            row(SM_DEV, "snapmaker", "U1-8110026", ip="192.168.77.40")],
               "configured": [CONFIGURED, PH_DEV, SM_DEV],
               "configured_unsure_kinds": ["bambu_cloud"]}, f)
with open(os.path.join(DD, "hub", "print_host_devices.json"), "w", encoding="utf-8") as f:
    json.dump({"version": 1, "models": {"Gate Qidi X-Max 4": {"devices": [
        {"id": "gatedev01", "alias": "Test", "address": "127.0.0.32", "host_type": "octoprint"},
        {"id": "gatedev02", "alias": "Keep", "address": "127.0.0.33", "host_type": "octoprint"}]}}}, f)
with open(os.path.join(DD, "hub", "snapmaker_lan.json"), "w", encoding="utf-8") as f:
    json.dump({"devices": [{"id": "GATE-U1-SN", "name": "U1-8110026", "model": "Snapmaker U1", "ip": "192.168.77.40",
                            "port": 80, "added_by": "discovery"}]}, f)


def read_json(name):
    try:
        with open(os.path.join(DD, "hub", name), encoding="utf-8") as f:
            return json.load(f)
    except Exception:  # noqa
        return {}

proc, hj = start_hub()
try:
    LOOP = "http://127.0.0.1:%d" % hj["port"]
    ADM = "http://127.0.0.1:%d" % hj["admin_port"]
    R = LOOP + "/r/" + hj["token"]
    J = {"Content-Type": "application/json"}
    H = {"X-Hub-Secret": hj["secret"], "Content-Type": "application/json"}
    print("hub pid %s on %d (admin %d)" % (hj["pid"], hj["port"], hj["admin_port"]))

    print("F1 the prune on load")
    rows = summary_rows(R)
    check(CONFIGURED in rows, "the configured (offline) printer stays")
    check(LEFTOVER not in rows, "the leftover test printer is gone")
    check(CLOUD_A in rows and CLOUD_B in rows, "cloud Bambu rows the last window could not judge stay")
    check(LEFTOVER not in saved_ids() and CONFIGURED in saved_ids(), "printers.json was rewritten without the leftover")

    print("F2 the fields and the feature")
    check(rows.get(CONFIGURED, {}).get("configured") is True, "/summary: configured true on the set-up printer")
    check(rows.get(CLOUD_A, {}).get("remove", {}).get("mode") == "explain" and rows.get(CLOUD_A, {}).get("remove", {}).get("kind") == "bambu_cloud",
          "/summary: a cloud-bound Bambu row says Remove only explains (%s)" % rows.get(CLOUD_A, {}).get("remove"))
    check(rows.get(PH_DEV, {}).get("remove", {}).get("kind") == "print_host_device" and rows.get(SM_DEV, {}).get("remove", {}).get("kind") == "lan_snapmaker",
          "/summary: remove kinds for a print host and a LAN Snapmaker")
    st, b = http(R + "/pair")
    feats = json.loads(b).get("features", []) if st == 200 else []
    check(st == 200 and "forget_printer" in feats, "/pair lists forget_printer (%s)" % st)

    print("F3 the phone plane")
    url = lambda pid: R + "/printers/" + urllib.parse.quote(pid, safe="") + "/forget"
    st, _ = http(url(CONFIGURED), "POST", {"Content-Type": "text/plain"}, b"{}")
    check(st == 415, "a non-JSON POST is refused (%s)" % st)
    st, _ = http(url("sm:NOT-THERE"), "POST", J, b"{}")
    check(st == 404, "an unknown id is 404 (%s)" % st)
    st, b = http(url(CLOUD_A), "POST", J, b"{}")
    check(st == 409 and json.loads(b).get("reason_kind") == "bambu_cloud", "a cloud-bound Bambu row is refused with 409 bambu_cloud (%s %s)" % (st, b[:80]))
    check(CLOUD_A in summary_rows(R), "... and stays")
    st, b = http(url(PH_DEV), "POST", J, b"{}")
    d = json.loads(b) if st == 200 else {}
    check(st == 200 and "print_host_devices" in d.get("done", []), "a configured print host is removed at its source (%s %s)" % (st, b[:120]))
    devs = [x.get("id") for x in read_json("print_host_devices.json").get("models", {}).get("Gate Qidi X-Max 4", {}).get("devices", [])]
    check(devs == ["gatedev02"], "... it left print_host_devices.json, the other device stayed (%s)" % devs)
    st, b = http(url(SM_DEV), "POST", J, b"{}")
    d = json.loads(b) if st == 200 else {}
    check(st == 200 and "snapmaker_lan" in d.get("done", []) and "tombstone" in d.get("done", []), "a LAN Snapmaker is removed and tombstoned (%s %s)" % (st, b[:120]))
    check(read_json("snapmaker_lan.json").get("devices", []) == [], "... it left snapmaker_lan.json")
    tomb = read_json("removed_printers.json").get("removed", [])
    check(any(t.get("id") == SM_DEV and t.get("ip") == "192.168.77.40" for t in tomb), "... and removed_printers.json names it by id and address (%s)" % tomb)
    st, b = http(url(CONFIGURED), "POST", J, b"{}")
    check(st == 200, "an offline configured Snapmaker can be removed too (%s)" % st)
    rows = summary_rows(R)
    check(not ({PH_DEV, SM_DEV, CONFIGURED} & set(rows)), "... all three are gone from /summary (%s)" % sorted(rows))
    check(not ({PH_DEV, SM_DEV, CONFIGURED} & saved_ids()), "... and from printers.json")

    print("F4 the control plane")
    st, _ = http(ADM + "/hub/printers")
    check(st == 403, "GET /hub/printers without the secret is 403 (%s)" % st)
    st, b = http(ADM + "/hub/printers", "GET", {"X-Hub-Secret": hj["secret"]})
    ids = {p.get("id") for p in json.loads(b).get("printers", [])} if st == 200 else set()
    check(st == 200 and ids == {CLOUD_A, CLOUD_B}, "GET /hub/printers lists the remembered rows (%s %s)" % (st, sorted(ids)))
    adm_url = ADM + "/hub/printers/" + urllib.parse.quote(CLOUD_B, safe="") + "/forget"
    st, _ = http(adm_url, "POST", J, b"{}")
    check(st == 403, "POST .../forget without the secret is 403 (%s)" % st)
    st, _ = http(ADM + "/hub/printers/" + urllib.parse.quote("sm:NOT-THERE", safe="") + "/forget", "POST", H, b"{}")
    check(st == 404, "the control plane answers 404 for an unknown id (%s)" % st)
    st, _ = http(adm_url, "POST", H, b"{}")
    check(st == 200, "the control plane forgets a leftover (%s)" % st)
    check(set(summary_rows(R)) == {CLOUD_A}, "only the cloud-bound Bambu printer is left")
finally:
    stop_hub(proc, hj)

print("F5 a restart keeps it that way")
proc, hj = start_hub()
try:
    R = "http://127.0.0.1:%d/r/%s" % (hj["port"], hj["token"])
    check(set(summary_rows(R)) == {CLOUD_A}, "after a hub restart the removed printers stay gone")
    check(any(t.get("id") == SM_DEV for t in read_json("removed_printers.json").get("removed", [])), "... and the tombstone is still there")
finally:
    stop_hub(proc, hj)

print("\nHUB FORGET GATE: %s (%d failed)" % ("PASS" if not fails else "FAIL", len(fails)))
for f in fails:
    print("  - " + f)
sys.exit(1 if fails else 0)

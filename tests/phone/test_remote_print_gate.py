#!/usr/bin/env python3
"""End-to-end gate for remote printing (research 2b, A1 + A2): another PC ("slicer B") sends a
pre-sliced .gcode.3mf to this PC's hub with a client token, and this PC prints it over the LAN
through the existing Reprint path - against UltraNet's loopback fake printer, never a real one.

The run (a hub on the gate's data dir, NO slicer instance running - the hub must start one):
  1. the hub page makes two clients: "Remote PC" (status + print) and "Viewer" (status only);
  2. uploads that are refused before anything is read: the viewer's (403), a name that is not a
     .gcode.3mf (400), a body over 500 MB (413), a wrong token (404);
  3. the real upload (/h/api/archive/upload, Authorization: Bearer) of the H2D fixture: a hidden
     slicer is started for it (A2), and the job becomes an archive record with source "remote",
     the client's name, the model it was sliced for, and no path on this PC;
  4. /api/instances/ensure finds that same instance; files that are not sliced jobs (text, a
     project with no G-code, an entry named ../..) are refused by the instance's checks and leave
     nothing in the archive or the hub's upload folder;
  5. the mapping sheet (/h/i/{pid}/api/archive/{id}/preview) - automatic mapping, then the
     client's own mapping - and the send with that override: the fake printer gets the archived
     bytes and a project_file whose ams_mapping is the override;
  6. the Reprint list shows the record (source remote).

usage: test_remote_print_gate.py --exe <install>/EdgeSlicer.exe --datadir DIR
                                 --fakeprinter <ultranet>/tools/fakeprinter.py
                                 --fixtures <repo>/tests/data/bambu_reprint --work DIR
The data dir is prepared like the Bambu reprint gate's (only fake printers on 127.0.0.1 with access
codes, the UltraNet plug-in, the archive switched on, phone link testtoken12345, phone access off)
and no slicer instance may be running on it. Ports 8883 / 990 must be free (one fake at a time).
Only processes this script started (the hub, the fake) - and the hidden slicer the hub started for
it, found through the hub - are ever stopped, by PID. %APPDATA%\\EdgeSlicer is never touched.
"""
import argparse
import hashlib
import io
import json
import os
import socket
import subprocess
import sys
import time
import types
import urllib.error
import urllib.parse
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's folder out; the fake printer driver lives next to it
from test_bambu_reprint_gate import Fake  # noqa: E402

TOKEN = "testtoken12345"
WIN = os.name == "nt"
FAILS = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)
    return cond


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest()


if WIN:
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]

    def end_pid(pid, wait_s):
        """Wait for `pid` to exit; terminate it after wait_s. Only for a process this gate caused."""
        h = k32.OpenProcess(0x00100000 | 0x0001, False, pid)  # SYNCHRONIZE | PROCESS_TERMINATE
        if not h:
            return
        try:
            if k32.WaitForSingleObject(h, int(wait_s * 1000)) != 0:
                print("ending pid %d" % pid, flush=True)
                k32.TerminateProcess(h, 1)
        finally:
            k32.CloseHandle(h)
else:
    import signal

    def end_pid(pid, wait_s):
        deadline = time.time() + wait_s
        while time.time() < deadline:
            try:
                os.kill(pid, 0)
            except OSError:
                return
            time.sleep(0.5)
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


class Hub:
    def __init__(self, hj):
        self.port, self.admin, self.secret = hj["port"], hj["admin_port"], hj.get("secret", "")

    def call(self, path, method="GET", headers=None, body=None, admin=False, timeout=60):
        url = "http://127.0.0.1:%d%s" % (self.admin if admin else self.port, path)
        h = dict(headers or {})
        if admin:
            h["X-Hub-Secret"] = self.secret
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            h.setdefault("Content-Type", "application/json")
        if method == "POST" and body is None:
            body = b"{}"
        req = urllib.request.Request(url, data=body, method=method, headers=h)
        try:
            r = urllib.request.urlopen(req, timeout=timeout)
            text = r.read().decode("utf-8", "replace")
            status = r.status
        except urllib.error.HTTPError as e:
            text, status = e.read().decode("utf-8", "replace"), e.code
        try:
            return status, json.loads(text or "{}")
        except ValueError:
            return status, {"error": text}

    def form(self, path, token, fields, timeout=120):
        body = urllib.parse.urlencode(fields).encode()
        return self.call(path, "POST", {"Authorization": "Bearer " + token, "Content-Type": "application/x-www-form-urlencoded"},
                         body, timeout=timeout)

    def upload(self, token, name, data, printer="", timeout=420):
        q = "?printer=" + urllib.parse.quote(printer) if printer else ""
        return self.call("/h/api/archive/upload" + q, "POST",
                         {"Authorization": "Bearer " + token, "X-File-Name": urllib.parse.quote(name),
                          "Content-Type": "application/octet-stream"}, data, timeout=timeout)

    def follow(self, token, pid, job, budget=240):
        deadline = time.time() + budget
        while time.time() < deadline:
            st, j = self.call("/h/i/%d/api/jobs/%d" % (pid, job), headers={"Authorization": "Bearer " + token})
            if st == 200 and j.get("state") != "running":
                return j
            time.sleep(1)
        return {"state": "timeout"}


def raw_oversize(port, token):
    """A request head claiming 600 MB, and no body: the hub must refuse before reading any."""
    s = socket.create_connection(("127.0.0.1", port), timeout=20)
    try:
        s.sendall(("POST /h/api/archive/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer %s\r\n"
                   "X-File-Name: big.gcode.3mf\r\nContent-Length: 600000000\r\n\r\n" % token).encode())
        data = b""
        while b"\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return int(data.split(b" ")[1]) if data.startswith(b"HTTP/") else 0
    finally:
        s.close()


def zip_bytes(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            z.writestr(name, data)
    return buf.getvalue()


def archive_files(datadir):
    d = os.path.join(datadir, "gcode_archive")
    return sorted(os.listdir(d)) if os.path.isdir(d) else []


def run(args, hub):
    st, j = hub.call("/hub/clients", "POST", body={"name": "Remote PC", "scopes": ["status", "print"]}, admin=True)
    remote = j.get("token", "")
    check(st == 200 and len(remote) >= 20, "the hub page makes a status + print client")
    st, j = hub.call("/hub/clients", "POST", body={"name": "Viewer", "scopes": ["status"]}, admin=True)
    viewer = j.get("token", "")
    check(st == 200 and viewer, "... and a status-only one")

    st, j = hub.call("/h/api/instances", headers={"Authorization": "Bearer " + remote})
    check(st == 200 and not j.get("instances"), "no slicer instance runs on the data dir yet (%s)" % j)

    fixture = os.path.join(args.fixtures, "h2d_two_sided.gcode.3mf")
    data = open(fixture, "rb").read()
    before = archive_files(args.datadir)
    st, j = hub.upload(viewer, "two_sided_plate_2.gcode.3mf", data, "SNFAKEH2D0001")
    check(st == 403 and "print" in j.get("error", ""), "a status-only client cannot upload (403: %s)" % j.get("error"))
    st, j = hub.upload(remote, "model.stl", data)
    check(st == 400 and ".gcode.3mf" in j.get("error", ""), "a name that is not .gcode.3mf is refused (%s)" % j.get("error"))
    check(raw_oversize(hub.port, remote) == 413, "a body over 500 MB is refused before it is read (413)")
    st, j = hub.upload("wrongtokenwrongtokenwrong1", "two_sided_plate_2.gcode.3mf", data)
    check(st == 404, "a wrong token is a 404 (%s)" % st)
    st, j = hub.call("/h/api/instances", headers={"Authorization": "Bearer " + remote})
    check(st == 200 and not j.get("instances"), "none of the refusals started a slicer")

    fake = Fake(args, "SNFAKEH2D0001", "h2d")
    try:
        t0 = time.time()
        st, up = hub.upload(remote, "two_sided_plate_2.gcode.3mf", data, "SNFAKEH2D0001")
        print("upload: %s in %.0f s: %s" % (st, time.time() - t0, json.dumps(up)[:600]), flush=True)
        if not check(st == 200, "the sliced job is accepted (%s)" % up.get("error")):
            return
        rec, pid = up.get("record", {}), up.get("instance", 0)
        rid = rec.get("id", "")
        check(up.get("spawned") is True and pid > 0, "a hidden slicer was started for it (A2): pid %s" % pid)
        check(rec.get("source") == "remote" and rec.get("source_client") == "Remote PC",
              "the record says it came from another PC, and which client (%s, %s)" % (rec.get("source"), rec.get("source_client")))
        check(rec.get("printer", {}).get("id") == "SNFAKEH2D0001" and rec.get("printer", {}).get("kind") == "bambu",
              "the record names the printer it is meant for")
        check("H2D" in rec.get("printer", {}).get("model", ""), "the record knows the model it was sliced for (%s)" % rec.get("printer"))
        check(rec.get("exists") is True and rec.get("sent_name") == "two_sided_plate_2.gcode.3mf", "the file is stored under its own name")
        check("path" not in rec and "project_path" not in rec, "no path on this PC reaches the client")
        check(up.get("preview") == "i/%d/api/archive/%s/preview" % (pid, rid), "the answer names the preview route")
        stored = [f for f in archive_files(args.datadir) if f not in before]
        check(any(f.endswith(".gcode.3mf") for f in stored), "the job is in the G-code archive (%s)" % stored)
        st, j = hub.call("/h/api/instances", headers={"Authorization": "Bearer " + remote})
        insts = j.get("instances", [])
        check(st == 200 and len(insts) == 1 and insts[0].get("hidden") is True, "one instance runs, hidden (%s)" % j)
        st, j = hub.call("/h/api/instances/ensure", "POST", {"Authorization": "Bearer " + remote})
        check(st == 200 and j.get("instance") == pid and j.get("spawned") is False, "ensure finds the same instance (%s)" % j)
        uploads = os.path.join(args.datadir, "hub", "uploads")
        check(not os.path.isdir(uploads) or not os.listdir(uploads), "the hub's spooled copy is gone (the archive keeps its own)")

        # Files that are not sliced jobs: refused by the instance's checks, nothing stored.
        before = archive_files(args.datadir)
        for what, body in (("a text file", b"G28\nG1 X10\n" * 10),
                           ("a project with no G-code", zip_bytes([("3D/3dmodel.model", "<model/>")])),
                           ("an entry named ../..", zip_bytes([("Metadata/plate_1.gcode", "G28\n"), ("../../evil.txt", "x")]))):
            st, j = hub.upload(remote, "bad.gcode.3mf", body, "SNFAKEH2D0001")
            check(st == 400, "%s is refused (%s: %s)" % (what, st, j.get("error")))
        check(archive_files(args.datadir) == before, "nothing refused reached the archive")
        check(not os.path.isdir(uploads) or not os.listdir(uploads), "nothing refused stays in the upload folder")

        # The mapping sheet, then the client's own mapping.
        base = "/h/i/%d/api/archive/%s" % (pid, rid)
        deadline, pv = time.time() + 90, {}
        while time.time() < deadline:
            st, pv = hub.form(base + "/preview", remote, {"printer": "SNFAKEH2D0001"})
            if st == 409 and any(w in pv.get("error", "") for w in ("storage", "SD card", "offline")):
                time.sleep(3)
                continue
            break
        print("preview:", st, json.dumps(pv)[:500], flush=True)
        if not check(st == 200 and pv.get("can_send") is True, "the mapping sheet answers and the job can be sent (%s)" % pv.get("error")):
            return
        check(pv.get("mapping") == "0:128-0,1:0-1,3:0-2", "automatic mapping as the phone's reprint gets it (%s)" % pv.get("mapping"))
        override = "0:128-0,1:0-1,3:0-0"
        st, pv2 = hub.form(base + "/preview", remote, {"printer": "SNFAKEH2D0001", "ams_mapping": override})
        check(st == 200 and pv2.get("mapping") == override and pv2.get("can_send") is True,
              "the client's own mapping is taken (%s, %s)" % (pv2.get("mapping"), pv2.get("problems")))

        uploads_before = len(fake.rec().get("uploads", []))
        st, j = hub.form(base + "/send", remote, {"printer": "SNFAKEH2D0001", "mode": "print", "confirm": "1", "ams_mapping": override})
        if not check(st == 200, "the send starts (%s %s)" % (st, j)):
            return
        done = hub.follow(remote, pid, j["job"])
        check(done.get("state") == "done", "the send finishes (%s: %s)" % (done.get("state"), done.get("error")))
        time.sleep(1)
        ups = [u for u in fake.rec().get("uploads", [])[uploads_before:] if "verify_job" not in u["name"]]
        check(len(ups) == 1 and ups[0]["md5"] == md5(fixture) and ups[0]["size"] == len(data),
              "the printer got the job byte for byte (%s)" % ups)
        pf = fake.payloads("project_file")
        if check(len(pf) == 1, "one project_file reached the printer"):
            p = pf[0]["print"]
            check(p.get("param") == "Metadata/plate_2.gcode", "project_file: plate 2 (%s)" % p.get("param"))
            check(p.get("ams_mapping") == [512, 1, -1, 0], "project_file: the client's mapping (%s)" % p.get("ams_mapping"))
            check(p.get("subtask_name") == "two_sided_plate_2", "project_file: the job's own name (%s)" % p.get("subtask_name"))

        st, j = hub.call("/h/api/archive", headers={"Authorization": "Bearer " + remote})
        row = [r for r in j.get("records", []) if r.get("id") == rid]
        check(st == 200 and row and row[0].get("source") == "remote", "the Reprint list shows the record as from another PC")
        st, j = hub.call("/h/api/archive", headers={"Authorization": "Bearer " + viewer})
        check(st == 200, "a status-only client may read the list")
        st, j = hub.form(base + "/send", viewer, {"printer": "SNFAKEH2D0001", "mode": "print", "confirm": "1"})
        check(st == 403, "... but not send (%s)" % st)
    finally:
        fake.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--fakeprinter", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    inst_dir = os.path.join(args.datadir, "hub", "instances")
    if os.path.isdir(inst_dir) and os.listdir(inst_dir):
        print("the data dir already has slicer instances registered; this gate needs none running")
        return 2
    hub_json = os.path.join(args.datadir, "hub", "hub.json")
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if WIN else 0
    env = dict(os.environ)
    env.pop("SNORCA_PHONE_ACCESS", None)
    p = subprocess.Popen([args.exe, "--hub", "--datadir", args.datadir], env=env, creationflags=flags,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=not WIN)
    print("hub pid", p.pid, flush=True)
    hj, hub = {}, None
    try:
        t0 = time.time()
        while time.time() - t0 < 90 and p.poll() is None:
            try:
                hj = json.load(open(hub_json, encoding="utf-8"))
                if hj.get("pid") == p.pid and hj.get("port") and hj.get("admin_port"):
                    break
            except Exception:
                pass
            time.sleep(0.3)
        if not check(hj.get("pid") == p.pid and hj.get("token") == TOKEN, "the hub starts on the gate's data dir"):
            return 1
        hub = Hub(hj)
        run(args, hub)
    finally:
        # The slicer the hub started for us: asked to quit (discarding its empty project), then by PID.
        pids = []
        try:
            pids = [int(f.split(".")[0]) for f in os.listdir(inst_dir) if f.endswith(".json")]
        except OSError:
            pass
        for pid in pids:
            if hub:
                hub.call("/hub/instances/%d/quit?discard=1" % pid, "POST", admin=True, timeout=30)
            end_pid(pid, 30)
        if p.poll() is None and hub:
            hub.call("/hub/quit", "POST", admin=True, timeout=10)
            try:
                p.wait(20)
            except subprocess.TimeoutExpired:
                pass
        if p.poll() is None:
            p.kill()
    print("\n%d failure(s)" % len(FAILS))
    for f in FAILS:
        print("  FAIL", f)
    print("GATE_REMOTE_PRINT=" + ("PASS" if not FAILS else "FAIL"))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())

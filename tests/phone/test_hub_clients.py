#!/usr/bin/env python3
"""Loopback gate: per-client hub tokens with scopes (feat/hub-scoped-tokens, HubClients.hpp).

Starts a test hub of its own on a new temporary data dir (`<exe> --hub --datadir <dir>`, phone
access off, loopback only) with a seeded phone link and one allowed tailnet login, then:

  - makes two clients on the control plane (/hub/clients): "status only" and "status + camera +
    print, tagged devices allowed"; the token comes back once, settings.json keeps only its hash
  - the phone link (legacy) still opens everything, by path, as before
  - a client token works in the path (/r/<token>/...) and as Authorization: Bearer on /h/...
  - a route outside a client's scopes answers 403 naming the permission; inside them it passes
  - nothing the hub builds for a client token contains the phone link's token
  - Tailscale Serve: a tagged device (Serve marks, no login) gets in only with the client that
    allows it; Funnel never does
  - revoking a client ends it at once; the phone link cannot be revoked
  - wrong tokens: 20 from one tailnet login block that login (429) while others carry on
  - uploads / prints: a client over 20 at once gets 429
  - hub.log has the client's connect / scope-refusal / revoke lines and never a token

Only the process this script started is stopped (control-plane quit, then by PID). A running hub
(the owner's) is never touched; %APPDATA%\\EdgeSlicer is never read or written; no printer or
Tailscale is involved.

Usage (from the repo root):
  sh /c/Dev/edgebuild/edgebuild gate-run -- python -I tests/phone/test_hub_clients.py C:/Dev/EdgeSlicerTest/<copy>/EdgeSlicer.exe
"""
import argparse, glob, json, os, shutil, subprocess, sys, tempfile, time, urllib.error, urllib.request

WIN = os.name == "nt"
TOKEN = "clientsgatetok01"
ALLOWED = "allowed@example.com"
SECOND = "second@example.com"
failures = []


def expect(what, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + what + (("  [" + str(detail) + "]") if detail else ""), flush=True)
    if not ok:
        failures.append(what)
    return ok


def hub_log_text(dd):
    text = ""
    for f in sorted(glob.glob(os.path.join(dd, "log", "hub.log*"))):
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                text += fh.read()
        except OSError:
            pass
    return text


class Hub:
    def __init__(self, hj):
        self.port, self.admin, self.secret = hj["port"], hj["admin_port"], hj.get("secret", "")

    def req(self, path, method="GET", headers=None, body=None, admin=False):
        url = "http://127.0.0.1:%d%s" % (self.admin if admin else self.port, path)
        h = dict(headers or {})
        if admin:
            h["X-Hub-Secret"] = self.secret
        if body is not None and not isinstance(body, bytes):
            body = json.dumps(body).encode()
            h.setdefault("Content-Type", "application/json")
        if method == "POST" and body is None:
            body = b"{}"
        r = urllib.request.Request(url, data=body, method=method, headers=h)
        try:
            resp = urllib.request.urlopen(r, timeout=30)
            return resp.status, resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")


def err(body):
    """The error text of a JSON error body (its quotes unescaped)."""
    try:
        return json.loads(body).get("error", "")
    except ValueError:
        return body


def serve(login=None, funnel=False):
    h = {"X-Forwarded-Host": "hub-pc.example.ts.net", "X-Forwarded-Proto": "https", "X-Forwarded-For": "100.64.0.7"}
    if login:
        h["Tailscale-User-Login"] = login
    if funnel:
        h["Tailscale-Funnel-Request"] = "?1"
    return h


def bearer(tok, extra=None):
    h = {"Authorization": "Bearer " + tok}
    h.update(extra or {})
    return h


def run(hub, dd):
    # ---- making clients ----
    st, body = hub.req("/hub/clients", "POST", body={"name": "Gate status", "scopes": ["status"]}, admin=True)
    a = json.loads(body) if st == 200 else {}
    expect("a status-only client is made and its token returned once", st == 200 and len(a.get("token", "")) >= 20, (st, body[:120]))
    st, body = hub.req("/hub/clients", "POST", body={"name": "Gate printer PC", "scopes": ["status", "camera", "print"], "tagged_ok": True}, admin=True)
    b = json.loads(body) if st == 200 else {}
    expect("a status+camera+print client (tagged allowed) is made", st == 200 and b.get("client", {}).get("tagged_ok") is True, (st, body[:120]))
    st, body = hub.req("/hub/clients", "POST", body={"name": "", "scopes": ["status"]}, admin=True)
    expect("a client needs a name", st == 400, st)
    st, body = hub.req("/hub/clients", "POST", body={"name": "Nothing", "scopes": ["root"]}, admin=True)
    expect("a client needs at least one known permission", st == 400, st)
    st, body = hub.req("/hub/clients", "POST", body={"name": "x", "scopes": ["status"]})
    expect("the main listener has no /hub/clients", st == 404, st)
    ta, tb = a.get("token", "x" * 26), b.get("token", "y" * 26)

    settings = open(os.path.join(dd, "hub", "settings.json"), encoding="utf-8").read()
    expect("settings.json keeps the clients' hashes, never their tokens",
           ta not in settings and tb not in settings and '"hash"' in settings and "Gate printer PC" in settings)
    st, body = hub.req("/hub/clients", admin=True)
    lst = json.loads(body).get("list", []) if st == 200 else []
    expect("the hub page lists the phone link first, then the two clients, with no hashes",
           [c.get("id") for c in lst][:1] == ["phone-link"] and len(lst) == 3 and '"hash"' not in body, body[:200])

    # ---- the phone link: unchanged ----
    st, _ = hub.req("/r/%s/summary" % TOKEN)
    expect("phone link: /summary 200", st == 200, st)
    st, _ = hub.req("/r/%s/api/instances/open" % TOKEN, "POST", body=b"")
    expect("phone link: an upload with no file is refused as a bad upload (400), not for permission", st == 400, st)

    # ---- client A (status) ----
    st, body = hub.req("/r/%s/summary" % ta)
    expect("status client by path: /summary 200", st == 200, st)
    expect("status client: /summary never contains the phone link's token", TOKEN not in body)
    st, body = hub.req("/r/%s/state" % ta)
    expect("status client: /state 200 and never contains the phone link's token", st == 200 and TOKEN not in body, st)
    st, body = hub.req("/r/%s/manifest.webmanifest" % ta)
    expect("status client: the manifest names its own link", st == 200 and TOKEN not in body and ta in body, st)
    st, body = hub.req("/r/%s/pair" % ta)
    expect("status client: /pair says what it may do", st == 200 and json.loads(body).get("scopes") == ["status"], body[:200])
    st, body = hub.req("/r/%s/pair" % TOKEN)
    expect("phone link: /pair lists every scope", st == 200 and len(json.loads(body).get("scopes", [])) == 4, st)
    st, body = hub.req("/r/%s/api/instances/open" % ta, "POST", body=b"")
    expect("status client: an upload is 403 and names the print permission", st == 403 and '"print"' in err(body), (st, body[:120]))
    st, body = hub.req("/r/%s/bambu?id=nothing" % ta)
    expect("status client: a camera is 403", st == 403 and '"camera"' in err(body), st)
    st, body = hub.req("/h/summary", headers=bearer(ta))
    expect("status client by header: /h/summary 200", st == 200, st)
    st, body = hub.req("/h/summary")
    expect("/h/ with no token: 404", st == 404, st)
    st, body = hub.req("/h/summary", headers=bearer("wrongtokenwrongtokenwrong1"))
    expect("/h/ with a wrong token: 404", st == 404, st)
    st, body = hub.req("/h/summary", headers=bearer(TOKEN))
    expect("/h/ with the phone link's token: 200", st == 200, st)

    # ---- client B (status + camera + print, tagged ok) ----
    st, body = hub.req("/h/api/instances/open", "POST", headers=bearer(tb), body=b"")
    expect("print client: an upload passes the permission check (400 for the empty upload)", st == 400, (st, body[:120]))
    st, body = hub.req("/h/i/1/api/printers/dev1/control", "POST", headers=bearer(tb), body=b"{}")
    expect("print client: a printer control is 403 (needs control)", st == 403 and '"control"' in err(body), (st, body[:120]))

    # ---- Tailscale Serve ----
    st, _ = hub.req("/h/summary", headers=bearer(ta, serve(login=ALLOWED)))
    expect("Serve + allowed login + status client: 200", st == 200, st)
    st, _ = hub.req("/h/summary", headers=bearer(tb, serve()))
    expect("tagged device + the client that allows it: 200", st == 200, st)
    st, _ = hub.req("/h/summary", headers=bearer(ta, serve()))
    expect("tagged device + a client that does not allow it: 403", st == 403, st)
    st, _ = hub.req("/r/%s/summary" % TOKEN, headers=serve())
    expect("tagged device + the phone link: 403", st == 403, st)
    st, _ = hub.req("/h/summary", headers=bearer(tb, serve(funnel=True)))
    expect("Funnel + the client that allows tagged devices: still 403", st == 403, st)

    # ---- revoke ----
    st, body = hub.req("/hub/clients/revoke?id=%s" % a["client"]["id"] if a else "/hub/clients/revoke?id=none", "POST", admin=True)
    expect("revoking the status client", st == 200, st)
    st, _ = hub.req("/r/%s/summary" % ta)
    expect("a revoked client's token: 404 at once", st == 404, st)
    st, _ = hub.req("/hub/clients/revoke?id=phone-link", "POST", admin=True)
    expect("the phone link cannot be revoked here", st == 400, st)

    # ---- limits ----
    st, _ = hub.req("/hub/remote?add=" + SECOND, "POST", admin=True)
    for i in range(20):
        hub.req("/r/wrongtoken%04d/summary" % i, headers=serve(login=SECOND))
    st, body = hub.req("/r/%s/summary" % TOKEN, headers=serve(login=SECOND))
    expect("20 wrong tokens from one tailnet login: that login gets 429, even with the right token", st == 429, st)
    st, _ = hub.req("/r/%s/summary" % TOKEN, headers=serve(login=ALLOWED))
    expect("... while another login carries on", st == 200, st)
    st, _ = hub.req("/r/%s/summary" % TOKEN)
    expect("... and this PC itself is never limited", st == 200, st)

    codes = [hub.req("/h/api/instances/open", "POST", headers=bearer(tb, serve(login=ALLOWED)), body=b"")[0] for _ in range(22)]
    expect("uploads: the print client's burst is 20, then 429", codes[:19].count(400) == 19 and codes[-1] == 429, codes)
    return [ta, tb]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if not os.path.isfile(exe):
        print("no such file:", exe)
        return 2
    dd = tempfile.mkdtemp(prefix="hubclients_")
    os.makedirs(os.path.join(dd, "hub"), exist_ok=True)
    with open(os.path.join(dd, "hub", "settings.json"), "w", encoding="utf-8") as fh:
        json.dump({"token": TOKEN, "phone": False, "remote_on": False, "allowed_logins": [ALLOWED]}, fh)
    hub_json = os.path.join(dd, "hub", "hub.json")
    env = dict(os.environ)
    env.pop("SNORCA_PHONE_ACCESS", None)
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if WIN else 0
    p = subprocess.Popen([exe, "--hub", "--datadir", dd], env=env, creationflags=flags,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=not WIN)
    print("hub pid", p.pid, "data dir", dd, flush=True)
    hj, ok_run = {}, False
    try:
        t0 = time.time()
        while time.time() - t0 < 90 and p.poll() is None:
            try:
                with open(hub_json, encoding="utf-8") as fh:
                    hj = json.load(fh)
                if hj.get("pid") == p.pid and hj.get("port") and hj.get("admin_port"):
                    break
            except Exception:
                pass
            time.sleep(0.3)
        if not expect("the test hub starts with the seeded phone link", hj.get("pid") == p.pid and hj.get("token") == TOKEN):
            return 1
        made = run(Hub(hj), dd)
        t_log = time.time()  # hub.log is flushed every couple of seconds by the hub loop
        while time.time() - t_log < 15 and "wrong tokens from one source" not in hub_log_text(dd):
            time.sleep(0.5)
        log = hub_log_text(dd)
        expect("hub.log: a client connected", "RemoteHub: client c" in log and ") connected, may " in log)
        expect("hub.log: a scope refusal", ") may not POST" in log)
        expect("hub.log: the revoke", ") revoked" in log)
        expect("hub.log: the wrong-token block", "wrong tokens from one source" in log)
        expect("hub.log never names the phone link's token or a client's", TOKEN not in log and not any(t in log for t in made))
        ok_run = True
    finally:
        if p.poll() is None and hj.get("admin_port"):
            try:
                r = urllib.request.Request("http://127.0.0.1:%d/hub/quit" % hj["admin_port"], data=b"{}", method="POST",
                                           headers={"X-Hub-Secret": hj.get("secret", ""), "Content-Type": "application/json"})
                urllib.request.urlopen(r, timeout=5).read()
                p.wait(20)
            except Exception:
                pass
        if p.poll() is None:
            print("ending the test hub (pid %d)" % p.pid, flush=True)
            p.kill()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                pass
        if ok_run and not failures:
            shutil.rmtree(dd, ignore_errors=True)
        else:
            print("data dir kept for a look:", dd, flush=True)
    print("\n%s: %d failure(s)%s" % ("PASS" if not failures else "FAIL", len(failures),
                                     "" if not failures else ": " + "; ".join(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

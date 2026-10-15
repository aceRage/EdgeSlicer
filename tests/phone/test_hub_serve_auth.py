#!/usr/bin/env python3
"""Loopback gate: a request that arrives through Tailscale Serve with no tailnet login (a tagged
device, a Funnel visitor) is refused, instead of being let through on the link token alone.
(fix/hub-serve-allowlist)

Tailscale Serve terminates HTTPS and forwards to the hub on loopback. It marks what it forwards
(X-Forwarded-Host / -For, X-Forwarded-Proto: https, Tailscale-Funnel-Request: ?1 on Funnel) and adds
Tailscale-User-Login only for a signed-in tailnet user. This gate plays Serve from 127.0.0.1 against
a test hub of its own:

  - plain loopback (no Serve marks) with the token: 200, as before
  - Serve + an allow-listed login: 200, and /state names the login
  - Serve + a login not on the allow-list: 403 (as before)
  - Serve marks, no login (tagged device): 403 on /state, /summary, /events and the root page
  - Funnel (with and without a claimed login): 403
  - a wrong token is still 404 for a plain loopback request
  - hub.log says the request was refused, once (the line is rate-limited), and /hub/info's remote
    card counts every refusal

The test hub runs on a new temporary data dir (`<exe> --hub --datadir <dir>`), phone access off
(loopback only), with a settings.json this script writes first: a fixed test token and one allowed
login. It takes the first free port from 13640 up; a running hub (the owner's) is never touched,
and only the process this script started is stopped, by PID. %APPDATA%\\EdgeSlicer is never read
or written. No printer and no Tailscale is involved.

Usage (from the repo root):
  sh /c/Dev/edgebuild/edgebuild gate-run -- python -I tests/phone/test_hub_serve_auth.py C:/Dev/EdgeSlicerTest/<copy>/EdgeSlicer.exe
Exit code 0 when every expectation holds.
"""
import argparse, glob, json, os, shutil, subprocess, sys, tempfile, time, urllib.error, urllib.request

WIN = os.name == "nt"
TOKEN = "servegatetoken01"
ALLOWED = "allowed@example.com"
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


def get(port, path, headers=None):
    """(status, body text) of a GET on the hub's main listener from 127.0.0.1."""
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), headers=headers or {})
    try:
        r = urllib.request.urlopen(req, timeout=20)
        return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def serve_headers(login=None, funnel=False):
    """What Tailscale Serve puts on a request it forwards (the client's own copies are stripped)."""
    h = {"X-Forwarded-Host": "hub-pc.example.ts.net", "X-Forwarded-Proto": "https", "X-Forwarded-For": "100.64.0.7"}
    if login:
        h["Tailscale-User-Login"] = login
        h["Tailscale-User-Name"] = "Test User"
    if funnel:
        h["Tailscale-Funnel-Request"] = "?1"
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if not os.path.isfile(exe):
        print("no such file:", exe)
        return 2

    dd = tempfile.mkdtemp(prefix="hubserveauth_")
    hub_dir = os.path.join(dd, "hub")
    os.makedirs(hub_dir, exist_ok=True)
    with open(os.path.join(hub_dir, "settings.json"), "w", encoding="utf-8") as fh:
        json.dump({"token": TOKEN, "phone": False, "remote_on": False, "allowed_logins": [ALLOWED]}, fh)
    hub_json = os.path.join(hub_dir, "hub.json")
    env = dict(os.environ)
    env.pop("SNORCA_PHONE_ACCESS", None)
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if WIN else 0
    p = subprocess.Popen([exe, "--hub", "--datadir", dd], env=env, creationflags=flags,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=not WIN)
    print("hub pid", p.pid, "data dir", dd, flush=True)
    ok_run = False
    try:
        hj, t0 = {}, time.time()
        while time.time() - t0 < 90 and p.poll() is None:
            try:
                with open(hub_json, encoding="utf-8") as fh:
                    hj = json.load(fh)
                if hj.get("pid") == p.pid and hj.get("port") and hj.get("admin_port"):
                    break
            except Exception:
                pass
            time.sleep(0.3)
        if not expect("the test hub starts", hj.get("pid") == p.pid and bool(hj.get("port"))):
            return 1
        expect("the hub kept the seeded test token", hj.get("token") == TOKEN)
        port = hj["port"]
        base = "/r/%s" % TOKEN
        print("hub on port %d (control %d)" % (port, hj["admin_port"]), flush=True)

        st, body = get(port, base + "/state")
        expect("plain loopback with the token: 200", st == 200, st)
        st, body = get(port, "/r/wrongtoken0000/state")
        expect("plain loopback with a wrong token: 404", st == 404, st)

        st, body = get(port, base + "/state", serve_headers(login=ALLOWED))
        expect("Serve + an allow-listed login: 200", st == 200, st)
        try:
            remote_login = json.loads(body).get("remote_login")
        except ValueError:
            remote_login = None
        expect("/state names the tailnet login", remote_login == ALLOWED, remote_login)

        st, body = get(port, base + "/state", serve_headers(login="stranger@example.com"))
        expect("Serve + a login not on the allow-list: 403", st == 403 and "not shared" in body, st)

        refused = 0
        for path in ("/state", "/summary", "/events?since=0", "/"):
            st, body = get(port, base + path, serve_headers())
            refused += 1
            expect("Serve with no login (tagged device) on %s: 403" % path, st == 403 and "tagged" in body, st)
        st, body = get(port, base + "/state", serve_headers(funnel=True))
        refused += 1
        expect("Funnel request: 403", st == 403, st)
        st, body = get(port, base + "/state", serve_headers(login=ALLOWED, funnel=True))
        refused += 1
        expect("Funnel request claiming an allowed login: 403", st == 403, st)
        # Only X-Forwarded-For (some Serve versions send no X-Forwarded-Proto on plain HTTP).
        st, body = get(port, base + "/state", {"X-Forwarded-For": "100.64.0.9"})
        refused += 1
        expect("a forwarded request with only X-Forwarded-For and no login: 403", st == 403, st)

        st, body = get(port, base + "/state")
        expect("plain loopback still works after the refusals: 200", st == 200, st)

        time.sleep(1.0)  # let the log flush
        log = hub_log_text(dd)
        n_lines = log.count("refused a Tailscale Serve request with no tailnet login")
        expect("hub.log names the refusal", n_lines >= 1, n_lines)
        expect("hub.log does not log every refusal (rate-limited)", n_lines == 1, n_lines)
        expect("hub.log never names the token", TOKEN not in log)

        req = urllib.request.Request("http://127.0.0.1:%d/hub/info" % hj["admin_port"],
                                     headers={"X-Hub-Secret": hj.get("secret", "")})
        try:
            info = json.load(urllib.request.urlopen(req, timeout=60))
        except Exception as e:
            info = {"error": str(e)}
        rm = info.get("remote", {}) if isinstance(info, dict) else {}
        expect("the hub page's remote card counts the refusals", rm.get("refused_no_login") == refused,
               (rm.get("refused_no_login"), refused))
        expect("the remote card no longer offers a Funnel command", "funnel_command" not in rm and bool(rm.get("funnel_off_command")))
        ok_run = True
    finally:
        if p.poll() is None:
            # The control plane's own quit first; the PID only if it does not go.
            try:
                req = urllib.request.Request("http://127.0.0.1:%d/hub/quit" % hj["admin_port"], data=b"{}", method="POST",
                                             headers={"X-Hub-Secret": hj.get("secret", ""), "Content-Type": "application/json"})
                urllib.request.urlopen(req, timeout=5).read()
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

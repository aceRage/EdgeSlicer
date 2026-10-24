#!/usr/bin/env python3
"""EdgeSlicer printer emulator (phase E1: Moonraker / QIDI).

Local, fake printers that EdgeSlicer can be pointed at by IP. Every request is logged and decoded.
Python 3.8+, standard library only. Never contacts a real printer; binds to 127.0.0.x unless a
printer entry opts in to one private LAN address.

  emu.py [serve] [--home DIR] [--config FILE] [--control-port 8790] [--no-browser] [--no-save]
        Run the emulator and its control page (http://127.0.0.1:8790/). With an emulator already
        running on that port, just opens the page.

  Talking to a running emulator (all take --control http://127.0.0.1:8790):
  emu.py status                         printers, addresses, states
  emu.py presets                        preset names
  emu.py add <preset> [key=value ...]   add a printer from a preset (id=, bind=, port=, name= ...)
  emu.py set <id> key=value ...         dotted keys: state=printing faults.drop_save_variable=1
                                        slots.0.type=ASA qidi.boxes=0 (values are JSON when they parse)
  emu.py reset <id> [key=value ...]     forget files / prints / saved variables (after the patch)
  emu.py state <id> <standby|printing|paused|complete|cancelled|error>
  emu.py start|stop|restart|remove <id>
  emu.py log [<id>] [--since N] [--kind K] [--json]
  emu.py bundle [file.zip]              save the log bundle
  emu.py quit                           stop the emulator
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

DEFAULT_CONTROL_PORT = 8790


def default_home():
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~\\AppData\\Local")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "EdgeSlicerEmulator")


# ---- client side -------------------------------------------------------------------------------
def call(control, path, body=None, method=None, timeout=30, raw=False):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(control.rstrip("/") + path, data=data, method=method or ("POST" if body is not None else "GET"),
                                 headers={"Content-Type": "application/json"} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            out = r.read()
            return out if raw else json.loads(out or b"{}")
    except urllib.error.HTTPError as e:
        try:
            msg = json.loads(e.read()).get("error")
        except ValueError:
            msg = str(e)
        raise SystemExit("emulator: %s" % msg)
    except (urllib.error.URLError, OSError) as e:
        raise SystemExit("emulator not reachable at %s (%s). Start it with emu.py serve." % (control, e))


def parse_value(v):
    try:
        return json.loads(v)
    except ValueError:
        return v


def kv_patch(pairs):
    """["faults.drop_save_variable=1", "slots.0.type=ASA"] -> nested dict; a list index addresses
    an existing list entry (the server merges lists whole, so the caller sends the full list)."""
    patch = {}
    for pair in pairs:
        if "=" not in pair:
            raise SystemExit("expected key=value, got %r" % pair)
        k, v = pair.split("=", 1)
        keys = k.split(".")
        cur = patch
        for part in keys[:-1]:
            cur = cur.setdefault(part, {})
        cur[keys[-1]] = parse_value(v)
    return patch


def resolve_lists(patch, current):
    """slots.0.type=ASA arrives as {"slots": {"0": {"type": "ASA"}}}: rebuild the full list from
    the printer's current one."""
    out = {}
    for k, v in patch.items():
        cur = current.get(k)
        if isinstance(cur, list) and isinstance(v, dict) and all(x.isdigit() for x in v):
            lst = json.loads(json.dumps(cur))
            for idx, sub in v.items():
                i = int(idx)
                while len(lst) <= i:
                    lst.append({})
                if isinstance(sub, dict) and isinstance(lst[i], dict):
                    lst[i].update(sub)
                else:
                    lst[i] = sub
            out[k] = lst
        elif isinstance(v, dict) and isinstance(cur, dict):
            out[k] = resolve_lists(v, cur)
        else:
            out[k] = v
    return out


def printer_cfg(control, pid):
    for p in call(control, "/emu/printers")["printers"]:
        if p["id"] == pid:
            return p["config"]
    raise SystemExit("no printer '%s'" % pid)


def fmt_entry(e):
    pl = e.get("payload") or {}
    keys = ("var", "value", "cmd", "name", "file", "size", "objects", "fault", "refused", "error", "state")
    short = ", ".join("%s=%s" % (k, json.dumps(pl[k]) if not isinstance(pl[k], str) else pl[k]) for k in keys if k in pl)
    return "%5d %s %-12s %-14s %-4s %-32s %s %s" % (e["seq"], e["ts"][11:23], e["printer"], e["kind"], e.get("method", ""),
                                                   e.get("path", ""), e.get("status", ""), short)


def client(args, rest):
    c = args.control
    cmd = args.command
    if cmd == "status":
        r = call(c, "/emu/printers")
        print("home %s, config %s" % (r["home"], r["config"]))
        for p in r["printers"]:
            st = p["state"]
            print("%-16s %-10s %-8s %-28s %s%s" % (p["id"], p["personality"], st.get("print_state"),
                                                 ",".join(p["addresses"]) or "(stopped)",
                                                 p["config"].get("name", ""), "  ERROR: " + p["error"] if p["error"] else ""))
        return 0
    if cmd == "presets":
        for n, e in sorted(call(c, "/emu/presets")["presets"].items()):
            print("%-22s %-10s %s" % (n, e.get("personality"), e.get("name", "")))
        return 0
    if cmd == "add":
        if not rest:
            raise SystemExit("usage: emu.py add <preset> [key=value ...]")
        r = call(c, "/emu/printers/add", {"preset": rest[0], "overrides": kv_patch(rest[1:])})
        print("added %s on %s" % (r["id"], ",".join(r["addresses"]) or r["error"]))
        return 0
    if cmd in ("set", "reset"):
        if not rest:
            raise SystemExit("usage: emu.py %s <id> [key=value ...]" % cmd)
        pid = rest[0]
        patch = resolve_lists(kv_patch(rest[1:]), printer_cfg(c, pid)) if rest[1:] else {}
        if cmd == "set":
            if not patch:
                raise SystemExit("nothing to set")
            r = call(c, "/emu/printers/%s/set" % pid, patch)
            print(json.dumps(r["state"], indent=1))
        else:
            r = call(c, "/emu/printers/%s/reset" % pid, patch)
            print("reset %s; log mark %d" % (pid, r["mark"]))
        return 0
    if cmd == "state":
        if len(rest) != 2:
            raise SystemExit("usage: emu.py state <id> <state>")
        r = call(c, "/emu/printers/%s/state" % rest[0], {"state": rest[1]})
        print(r["state"]["print_state"])
        return 0
    if cmd in ("start", "stop", "restart", "remove"):
        if not rest:
            raise SystemExit("usage: emu.py %s <id>" % cmd)
        r = call(c, "/emu/printers/%s/%s" % (rest[0], cmd), {})
        print(json.dumps({k: r.get(k) for k in ("id", "running", "addresses", "error", "removed") if k in r}))
        return 0
    if cmd == "log":
        ap = argparse.ArgumentParser(prog="emu.py log")
        ap.add_argument("printer", nargs="?")
        ap.add_argument("--since", type=int, default=0)
        ap.add_argument("--kind")
        ap.add_argument("--json", action="store_true")
        la = ap.parse_args(rest)
        q = "/emu/log?since=%d" % la.since + ("&printer=" + la.printer if la.printer else "") + (
            "&kind=" + la.kind if la.kind else "")
        for e in call(c, q)["entries"]:
            print(json.dumps(e) if la.json else fmt_entry(e))
        return 0
    if cmd == "bundle":
        data = call(c, "/emu/bundle", raw=True)
        out = rest[0] if rest else "emulator_bundle_%s.zip" % time.strftime("%Y%m%d-%H%M%S")
        with open(out, "wb") as f:
            f.write(data)
        print("saved", out)
        return 0
    if cmd == "quit":
        call(c, "/emu/quit", {})
        print("quitting")
        return 0
    raise SystemExit("unknown command %s" % cmd)


# ---- server side -------------------------------------------------------------------------------
def serve(args):
    from core.control import ControlServer
    from core.supervisor import Supervisor

    url = "http://127.0.0.1:%d/" % args.control_port
    try:
        r = json.loads(urllib.request.urlopen(url + "emu/ping", timeout=2).read())
        if r.get("emulator"):
            print("An emulator is already running at %s (home %s); opening its page." % (url, r.get("home")))
            if not args.no_browser:
                webbrowser.open(url)
            return 0
    except (OSError, ValueError):
        pass
    home = os.path.abspath(args.home or default_home())
    sup = Supervisor(home, args.config, save=not args.no_save)
    try:
        ctl = ControlServer(sup, args.control_port)
    except OSError as e:
        from core import safety
        holder = safety.who_holds(args.control_port)
        print("Cannot open the control page on 127.0.0.1:%d (%s)%s" % (args.control_port, e,
                                                                      "; held by " + holder if holder else ""))
        return 2
    sup.start_all()
    print("EdgeSlicer printer emulator (E1)")
    print("  control page  %s" % url)
    print("  home          %s" % home)
    print("  config        %s" % sup.config_path)
    for p in sup.printers.values():
        h = p.personality.hint()
        print("  %-16s %-8s %s%s" % (p.id, p.personality.name, ", ".join(p.addresses()) or "(stopped)",
                                     "  ERROR: " + p.error if p.error else "  -> add in EdgeSlicer as %s, host %s"
                                     % (h["host_type"], h["address"])))
    print("READY control=%d" % args.control_port, flush=True)
    if not args.no_browser:
        try:
            webbrowser.open(url)
        except Exception:  # noqa: BLE001
            pass
    try:
        ctl.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        sup.stop_all()
        ctl.server_close()
    print("emulator stopped")
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(description="EdgeSlicer printer emulator (E1)", add_help=True)
    ap.add_argument("command", nargs="?", default="serve")
    ap.add_argument("--control", default=None, help="the running emulator, default http://127.0.0.1:8790")
    ap.add_argument("--control-port", type=int, default=DEFAULT_CONTROL_PORT)
    ap.add_argument("--home", help="config, logs and uploads (default %s)" % default_home())
    ap.add_argument("--config", help="printers.json (default <home>/printers.json)")
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--no-save", action="store_true", help="never write printers.json back")
    args, rest = ap.parse_known_args(argv)
    if args.control is None:
        args.control = "http://127.0.0.1:%d" % args.control_port
    if args.command == "serve":
        return serve(args)
    return client(args, rest)


if __name__ == "__main__":
    sys.exit(main())

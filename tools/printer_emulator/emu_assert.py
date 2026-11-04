#!/usr/bin/env python3
"""Assertions on the emulator's command log, for gates and hand checks. Standard library only.

    from emu_assert import Emulator
    emu = Emulator("http://127.0.0.1:8790")
    mark = emu.reset("qidi-xmax4", {"qidi": {"stale_value_t": {"0": "slot3"}}})
    # ... drive a send ...
    emu.expect("qidi-xmax4", kind="save_variable", var="enable_box", value=1, since=mark)
    emu.expect("qidi-xmax4", kind="save_variable", var="value_t0", value="slot0", since=mark)
    emu.expect_order("qidi-xmax4", ["save_variable", "readback", "upload", "start"], since=mark)
    emu.expect_none("qidi-xmax4", kind="upload", since=mark)
    emu.state("qidi-xmax4")["save_variables"]["value_t0"] == "slot0"

A field is matched against the entry's payload first, then the entry itself (kind, method, path,
status ...). Every failure raises EmuAssertionError with the log slice it looked at, so a red gate
explains itself.

CLI:
    emu_assert.py [--control URL] expect <printer> kind=save_variable var=value_t0 value=slot0 [--since N]
    emu_assert.py [--control URL] expect_none <printer> kind=upload [--since N]
    emu_assert.py [--control URL] expect_order <printer> save_variable,upload,start [--since N]
    emu_assert.py [--control URL] mark
"""
import json
import sys
import urllib.error
import urllib.request

_MISSING = object()


class EmuAssertionError(AssertionError):
    pass


def _short(e):
    pl = e.get("payload") or {}
    keep = {k: pl[k] for k in ("var", "value", "cmd", "name", "file", "size", "objects", "fault", "refused", "error",
                               "stored", "print") if k in pl}
    return "#%d %s %s %s %s %s" % (e["seq"], e["kind"], e.get("method", ""), e.get("path", ""), e.get("status", ""),
                                  json.dumps(keep))


def _field(entry, key):
    pl = entry.get("payload") or {}
    if key in pl:
        return pl[key]
    return entry.get(key, _MISSING)


def matches(entry, kind=None, **fields):
    if kind is not None and entry.get("kind") != kind:
        return False
    for k, want in fields.items():
        have = _field(entry, k)
        if have is _MISSING:
            return False
        if callable(want):
            if not want(have):
                return False
        elif have != want:
            return False
    return True


class Emulator:
    def __init__(self, control="http://127.0.0.1:8790", timeout=30):
        self.control = control.rstrip("/")
        self.timeout = timeout

    # ---- transport --------------------------------------------------------------------------
    def _call(self, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.control + path, data=data, method="POST" if data is not None else "GET",
                                     headers={"Content-Type": "application/json"} if data is not None else {})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            raise EmuAssertionError("emulator %s -> HTTP %d %s" % (path, e.code, e.read()[:300]))

    # ---- control ----------------------------------------------------------------------------
    def ping(self):
        return self._call("/emu/ping")

    def printers(self):
        return self._call("/emu/printers")["printers"]

    def printer(self, pid):
        for p in self.printers():
            if p["id"] == pid:
                return p
        raise EmuAssertionError("no emulated printer '%s'" % pid)

    def set(self, pid, patch):
        return self._call("/emu/printers/%s/set" % pid, patch)

    def reset(self, pid, patch=None, **kw):
        """Optional config patch (or keyword shortcuts: stale_value_t=, faults=, slots=, state=),
        then the runtime state is forgotten. Returns the log mark to pass as `since`."""
        patch = dict(patch or {})
        if "stale_value_t" in kw:
            patch.setdefault("qidi", {})["stale_value_t"] = kw.pop("stale_value_t")
        patch.update(kw)
        return self._call("/emu/printers/%s/reset" % pid, patch)["mark"]

    def set_state(self, pid, state):
        return self._call("/emu/printers/%s/state" % pid, {"state": state})

    def state(self, pid):
        return self._call("/emu/state?printer=" + pid)

    def mark(self):
        return self._call("/emu/mark")["mark"]

    def log(self, pid=None, since=0, kind=None):
        q = "/emu/log?since=%d" % since + ("&printer=" + pid if pid else "") + ("&kind=" + kind if kind else "")
        return self._call(q)["entries"]

    # ---- assertions -------------------------------------------------------------------------
    def _fail(self, msg, entries):
        lines = [msg, "log slice (%d entries):" % len(entries)] + ["  " + _short(e) for e in entries[-40:]]
        raise EmuAssertionError("\n".join(lines))

    def find(self, pid, kind=None, since=0, **fields):
        return [e for e in self.log(pid, since) if matches(e, kind, **fields)]

    def expect(self, pid, kind=None, since=0, count=None, **fields):
        """At least one matching entry (exactly `count` when given). Returns the first."""
        entries = self.log(pid, since)
        hit = [e for e in entries if matches(e, kind, **fields)]
        if not hit or (count is not None and len(hit) != count):
            self._fail("expected %s%s entry %s on %s, found %d" % ("" if count is None else "%d " % count, kind or "any",
                                                                   json.dumps(fields, default=str), pid, len(hit)), entries)
        return hit[0]

    def expect_none(self, pid, kind=None, since=0, **fields):
        entries = self.log(pid, since)
        hit = [e for e in entries if matches(e, kind, **fields)]
        if hit:
            self._fail("expected no %s entry %s on %s, found %d" % (kind or "", json.dumps(fields, default=str), pid,
                                                                    len(hit)), entries)

    def expect_order(self, pid, kinds, since=0):
        """The kinds appear in this order (as a subsequence; other entries may sit between)."""
        entries = self.log(pid, since)
        i = 0
        for e in entries:
            if i < len(kinds) and e["kind"] == kinds[i]:
                i += 1
        if i < len(kinds):
            self._fail("expected the order %s on %s; stopped at %r" % (kinds, pid, kinds[i]), entries)


def _cli(argv):
    control = "http://127.0.0.1:8790"
    since = 0
    args = []
    it = iter(argv)
    for a in it:
        if a == "--control":
            control = next(it)
        elif a == "--since":
            since = int(next(it))
        else:
            args.append(a)
    if not args:
        print(__doc__)
        return 2
    emu = Emulator(control)
    cmd = args[0]
    if cmd == "mark":
        print(emu.mark())
        return 0
    if len(args) < 2:
        print(__doc__)
        return 2
    pid = args[1]
    try:
        if cmd == "expect_order":
            emu.expect_order(pid, args[2].split(","), since)
        else:
            fields = {}
            for kv in args[2:]:
                k, _, v = kv.partition("=")
                try:
                    fields[k] = json.loads(v)
                except ValueError:
                    fields[k] = v
            kind = fields.pop("kind", None)
            if cmd == "expect":
                e = emu.expect(pid, kind, since, **fields)
                print(_short(e))
            elif cmd == "expect_none":
                emu.expect_none(pid, kind, since, **fields)
            else:
                print(__doc__)
                return 2
    except EmuAssertionError as e:
        print("FAIL: %s" % e)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))

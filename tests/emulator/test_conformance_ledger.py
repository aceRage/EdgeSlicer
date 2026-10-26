"""The conformance ledger (tools/printer_emulator/conformance/*.json): which fields EdgeSlicer reads
from and sends to each emulated printer, and whether the emulator provides them.

    python tests/emulator/test_conformance_ledger.py

Fails when
  * EdgeSlicer's source starts reading or sending a field the ledger does not list (the emulator
    would silently not test it: provide it and list it as provided, or list it as not_in_protocol /
    deferred with a reason);
  * a field listed as a provided READ is missing from every answer of a live emulator (a private one
    on loopback addresses, started here), or a provided SEND does not appear in the personality's
    source (nothing interprets it).
Ledger entries the source no longer has are reported (not failed), so the file can be tidied.
No EdgeSlicer build is needed; about 10 s.
"""
import json
import os
import re
import sys
import tempfile
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from conformance_scan import REPO, load_ledger, scan_reads, scan_sends  # noqa: E402
from emu_proc import EmulatorProcess, assert_loopback, free_port  # noqa: E402
from proto_clients import MqttClient, WsClient, elegoo_upload, http, is_response, multipart  # noqa: E402
import hashlib  # noqa: E402

PERSONALITIES = ("qidi", "elegoo_cc1", "u1", "elegoo_cc2")
fails, notes, checks = [], [], 0


def check(cond, what):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)
    return cond


def keys_of(obj, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.add(k)
            keys_of(v, out)
    elif isinstance(obj, list):
        for v in obj:
            keys_of(v, out)
    return out


def seen(key, keys, entry=None):
    """A ledger read key against the keys the emulator answered: dotted paths by their last part,
    '<n>' patterns, "prefix": true entries and trailing '_' prefixes by prefix + digits."""
    k = key.split(".")[-1]
    if (entry or {}).get("prefix"):
        k = k.rstrip("_") + ("_" if k.endswith("_") else "") + "<n>"
    if k.endswith("<n>"):
        stem = k[:-3]
        return any(re.fullmatch(re.escape(stem) + r"\d+", x) for x in keys) or (stem == "extruder" and "extruder" in keys)
    if k.endswith("_"):
        return any(re.fullmatch(re.escape(k) + r"\d+", x) for x in keys)
    return k in keys


GCODE = (b"; HEADER_BLOCK_START\n; HEADER_BLOCK_END\n; EXECUTABLE_BLOCK_START\nG28\n" + b"G1 X1\n" * 50 +
         b"; EXECUTABLE_BLOCK_END\n; estimated printing time (normal mode) = 12m 5s\n"
         b"; CONFIG_BLOCK_START\n; filament_type = PLA\n; CONFIG_BLOCK_END\n")


def live_keys(printers):
    """Every key a live emulator puts in its answers, per personality."""
    out = {p: set() for p in PERSONALITIES}
    q, c, u = printers["qidi"], printers["elegoo_cc1"], printers["u1"]

    def get_json(host, port, path, method="GET", body=None, ctype=None):
        st, raw = http(host, port, path, method, body, ctype)
        try:
            return json.loads(raw)
        except ValueError:
            return {}

    # QIDI: every object, the variables, a Klipper error.
    lst = get_json(q["bind"], q["port"], "/printer/objects/list")["result"]["objects"]
    out["qidi"] |= set(lst)
    keys_of(get_json(q["bind"], q["port"], "/printer/objects/query?" + "&".join(urllib.parse.quote(o) for o in lst)), out["qidi"])
    keys_of(get_json(q["bind"], q["port"], "/printer/gcode/script?script=NOPE%20X", "POST", b"{}", "application/json"), out["qidi"])

    # U1: the identity calls, every object, an upload's metadata, an error.
    for path in ("/server/info", "/access/info", "/printer/info", "/machine/system_info", "/printer/gcode/help"):
        keys_of(get_json(u["bind"], u["port"], path), out["u1"])
    lst = get_json(u["bind"], u["port"], "/printer/objects/list")["result"]["objects"]
    out["u1"] |= set(lst)
    body, ct = multipart({"print": "false", "root": "gcodes"}, "file", "c.gcode", GCODE)
    http(u["bind"], u["port"], "/server/files/upload", "POST", body, ct)
    http(u["bind"], u["port"], "/printer/print/start?filename=c.gcode", "POST", b"{}", "application/json")
    keys_of(get_json(u["bind"], u["port"], "/printer/objects/query?" + "&".join(urllib.parse.quote(o) for o in lst)), out["u1"])
    keys_of(get_json(u["bind"], u["port"], "/server/files/metadata?filename=c.gcode"), out["u1"])
    keys_of(get_json(u["bind"], u["port"], "/server/files/metadata?filename=nope.gcode"), out["u1"])

    # CC1: every SDCP frame, an upload answer and an upload error.
    w = WsClient(c["bind"])
    for cmd in (0, 1):
        w.cmd(cmd)
        keys_of(w.recv_json(want=lambda j: "Status" in j or "Attributes" in j), out["elegoo_cc1"])
    w.cmd(324)
    keys_of(w.recv_json(want=is_response(324)), out["elegoo_cc1"])
    keys_of(w.recv_json(want=is_response(324)), out["elegoo_cc1"])
    w.cmd(386, {"Enable": 1})
    keys_of(w.recv_json(want=is_response(386)), out["elegoo_cc1"])
    for s, j in elegoo_upload(c["bind"], 80, "c.gcode", GCODE, chunk=400) + elegoo_upload(c["bind"], 80, "x.gcode", GCODE, md5="0" * 32):
        keys_of(j, out["elegoo_cc1"])
    w.cmd(128, {"Filename": "/local/c.gcode"})
    keys_of(w.recv_json(want=is_response(128)), out["elegoo_cc1"])
    w.close()

    # CC2: /system/info, a PUT upload, MQTT registration, 1002, 2005 and 1020.
    c2 = printers["elegoo_cc2"]
    keys_of(get_json(c2["bind"], c2["port"], "/system/info?X-Token=123456"), out["elegoo_cc2"])
    md5 = hashlib.md5(GCODE).hexdigest()
    st, raw = http(c2["bind"], c2["port"], "/upload", "PUT", GCODE, "application/octet-stream",
                   headers={"Content-Range": "bytes 0-%d/%d" % (len(GCODE) - 1, len(GCODE)), "X-File-Name": "c.gcode",
                            "X-File-MD5": md5, "X-Token": "123456"})
    keys_of(json.loads(raw or b"{}"), out["elegoo_cc2"])
    sn = get_json(c2["bind"], c2["port"], "/system/info?X-Token=123456")["system_info"]["sn"]
    m = MqttClient(c2["bind"], 1883, "1_PC_5150", user="elegoo", password="123456")
    m.subscribe("elegoo/%s/1_PC_5150/api_response" % sn)
    m.subscribe("elegoo/%s/1_PC_5150_req/register_response" % sn)
    m.publish("elegoo/%s/api_register" % sn, json.dumps({"client_id": "1_PC_5150", "request_id": "1_PC_5150_req"}))
    keys_of(json.loads(m.next_message()[1]), out["elegoo_cc2"])
    for i, (method, params) in enumerate(((1002, {}), (2005, {}), (1020, {"storage_media": "local", "filename": "c.gcode",
                                                                          "config": {"slot_map": []}}))):
        m.publish("elegoo/%s/1_PC_5150/api_request" % sn, json.dumps({"id": 10 + i, "method": method, "params": params}))
        keys_of(json.loads(m.next_message()[1]), out["elegoo_cc2"])
    m.close()
    return out


def emulator_text(paths):
    text = ""
    for rel in paths:
        with open(os.path.join(REPO, rel), encoding="utf-8") as f:
            text += f.read()
    return text


def handled(send, text):
    s = send
    if s.startswith("form:"):
        s = s[5:]
    if s.startswith("Cmd "):
        n = s[4:]
        return re.search(r"\bcmd\s*(?:==|in)\s*\(?[^)\n]*\b%s\b" % n, text) is not None or ("Cmd %s" % n) in text
    if s.endswith("="):
        s = s[:-1]
    return re.search(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(s), text, re.I) is not None


def main():
    work = tempfile.mkdtemp(prefix="emu_conformance_")
    from core import safety
    cc1_host = next(h for h in ("127.0.0.%d" % n for n in range(240, 254))
                    if all(safety.port_free(h, p) for p in (80, 3030, 3031)))
    cc2_host = next(h for h in ("127.0.0.%d" % n for n in range(224, 240)) if safety.port_free(h, 1883))
    qh, uh = assert_loopback("127.0.0.%d" % (150 + os.getpid() % 30)), assert_loopback("127.0.0.%d" % (120 + os.getpid() % 30))
    printers = {
        "qidi": {"id": "cf-qidi", "personality": "qidi", "bind": qh, "port": free_port(qh), "qidi": {"boxes": 1, "polar_cooler": True, "stale_value_t": {"0": "slot1"}},
                 "slots": [{"type": "PLA", "color": "#E03030", "loaded": True}], "faults": {}},
        "elegoo_cc1": {"id": "cf-cc1", "personality": "elegoo_cc1", "bind": assert_loopback(cc1_host), "port": 80,
                       "slots": [{"type": "PLA", "color": "#E03030", "loaded": True}], "faults": {}},
        "u1": {"id": "cf-u1", "personality": "u1", "bind": uh, "port": free_port(uh),
               "slots": [{"type": "PLA", "color": "#E03030", "loaded": True}], "faults": {}},
        "elegoo_cc2": {"id": "cf-cc2", "personality": "elegoo_cc2", "bind": assert_loopback(cc2_host),
                       "port": free_port(cc2_host), "canvas": {"attached": True, "active_tray": 0},
                       "slots": [{"type": "PLA", "color": "#E03030", "loaded": True}], "faults": {}},
    }
    emu = EmulatorProcess(os.path.join(work, "emuhome"), list(printers.values()))
    try:
        emu.start()
        keys = live_keys(printers)
    finally:
        emu.stop()

    for p in PERSONALITIES:
        led = load_ledger(p)
        print("-- %s --" % p)
        statuses = ("provided", "not_in_protocol", "deferred")
        bad = [k for k, v in list(led["reads"].items()) + list(led["sends"].items()) if v.get("status") not in statuses]
        check(not bad, "%s: every ledger entry has a known status %s" % (p, bad or ""))
        reads = scan_reads(led["read_sources"])
        unlisted = sorted(k for k in reads if k not in led["reads"])
        check(not unlisted, "%s: EdgeSlicer reads no field the ledger leaves out%s" % (
            p, "" if not unlisted else ": " + ", ".join("%s (%s)" % (k, ", ".join(sorted(reads[k]))) for k in unlisted)))
        missing = sorted(k for k, v in led["reads"].items() if v["status"] == "provided" and not seen(k, keys[p], v))
        check(not missing, "%s: every provided read is in the live emulator's answers%s" % (p, "" if not missing else ": missing " + ", ".join(missing)))
        sends = scan_sends(led["send_sources"])
        unlisted = sorted(k for k in sends if k not in led["sends"])
        check(not unlisted, "%s: EdgeSlicer sends no field the ledger leaves out%s" % (
            p, "" if not unlisted else ": " + ", ".join("%s (%s)" % (k, ", ".join(sorted(sends[k]))) for k in unlisted)))
        text = emulator_text(led["emulator_sources"])
        unhandled = sorted(k for k, v in led["sends"].items() if v["status"] == "provided" and not handled(k, text))
        check(not unhandled, "%s: every provided send is interpreted by the emulator%s" % (p, "" if not unhandled else ": " + ", ".join(unhandled)))
        stale = sorted(set(led["reads"]) - set(reads)) + sorted(set(led["sends"]) - set(sends))
        if stale:
            notes.append("%s: in the ledger, no longer found in the source: %s" % (p, ", ".join(stale)))
        counts = {s: sum(1 for v in list(led["reads"].values()) + list(led["sends"].values()) if v["status"] == s) for s in statuses}
        print("     %s: %d reads, %d sends; %s" % (p, len(led["reads"]), len(led["sends"]), ", ".join("%s %d" % kv for kv in counts.items())))
    for n in notes:
        print("  note " + n)
    print("\n== conformance ledger: %d checks, %d failed ==" % (checks, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

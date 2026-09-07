"""Bar B for the Z-contouring (ZAA) port.

Slices the 40x40 mm wedge (2 mm -> 12 mm, 14.04 deg) at 0.2 mm layers with ZAA on
and checks the emitted G-code:

  * top-surface moves carry varying Z within a layer, inside the clamp;
  * per-segment E scales with the local height;
  * no Z ever below the layer's lo + zaa_min_z, nor above print_z + zaa_min_z;
  * a GCodeProcessor pass (--export-3mf) accepts it;
  * with offset_layers also on, wall Z = offset base + contour, still capped;
  * spiral vase on: contouring skipped, no error.

usage: bar_b_zaa.py --cand <exe> [--model <stl>]
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

T = os.path.dirname(os.path.abspath(__file__))
HUB = r"C:\Users\acesa\AppData\Local\Temp\snorca_hubtest"
SRC_DD = os.path.join(HUB, "dd_lan")
WORK = os.path.join(T, "bar_b_zaa_work")

PRINTER = "Bambu Lab P1S 0.4 nozzle"
PROCESS = "0.20mm Standard @BBL X1C"
FILAMENT = "Generic PLA"

LAYER_H = 0.2
MIN_Z = 0.05

RE_G1 = re.compile(r"^G[01]\s")
RE_TYPE = re.compile(r"^;\s*FEATURE:\s*(.*)$")
RE_Z_HEIGHT = re.compile(r"^;\s*Z_HEIGHT:\s*([0-9.]+)")


def num(tok):
    try:
        return float(tok)
    except ValueError:
        return None


def slice_once(exe, dd, out, extra, export3mf=False):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    cmd = [exe, "--slice", "0", "--allow-newer-file", "--no-thumbnails",
           "--datadir", dd, "--outputdir", out,
           "--printer-preset", PRINTER,
           "--process-preset", PROCESS,
           "--filament-presets", FILAMENT,
           "--layer-height=%s" % LAYER_H] + extra
    if export3mf:
        cmd += ["--export-3mf", "out.3mf"]
    cmd += [MODEL]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    so = p.stdout.decode("utf-8", "replace")
    se = p.stderr.decode("utf-8", "replace")
    files = sorted(os.path.join(out, f) for f in os.listdir(out) if f.endswith(".gcode"))
    return p.returncode, files[0] if files else None, so, se


def parse_moves(path):
    """-> list of (layer_print_z, type, x, y, z, e)"""
    moves = []
    cur_type = ""
    cur_z = None
    layer_z = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = RE_TYPE.match(line)
        if m:
            cur_type = m.group(1).strip()
            continue
        m = RE_Z_HEIGHT.match(line)
        if m:
            layer_z = float(m.group(1))
            continue
        if not RE_G1.match(line):
            continue
        x = y = z = e = None
        for tok in line.split(";")[0].split()[1:]:
            v = num(tok[1:])
            if v is None:
                continue
            if tok[0] == "X":
                x = v
            elif tok[0] == "Y":
                y = v
            elif tok[0] == "Z":
                z = v
            elif tok[0] == "E":
                e = v
        if z is not None:
            cur_z = z
        moves.append((layer_z, cur_type, x, y, cur_z, e))
    return moves


def report(name, gcode, offset_layers=False):
    """Delegate to bar_b_report.py, the rigorous analyzer."""
    cmd = [sys.executable, "-u", os.path.join(T, "bar_b_report.py"), gcode,
           "--layer-height=%s" % LAYER_H, "--min-z=%s" % MIN_Z]
    if offset_layers:
        cmd.append("--offset-layers")
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode("utf-8", "replace")
    return p.returncode == 0, [l for l in out.splitlines() if l.strip()]


def main():
    global MODEL
    ap = argparse.ArgumentParser()
    ap.add_argument("--cand", required=True)
    ap.add_argument("--model", default=os.path.join(T, "zaa_wedge.stl"))
    a = ap.parse_args()
    MODEL = a.model

    os.makedirs(WORK, exist_ok=True)
    dd = os.path.join(WORK, "dd")
    if not os.path.isdir(dd):
        shutil.copytree(SRC_DD, dd)

    print("cand : %s\nmodel: %s\n" % (a.cand, MODEL))
    rc = 0

    # ---------------------------------------------------------------- 1. ZAA off (reference)
    print("--- 1. ZAA off (reference)")
    r, f, so, se = slice_once(a.cand, dd, os.path.join(WORK, "off"), [])
    if r != 0 or not f:
        print("    FAIL rc=%s\n%s\n%s" % (r, so[-1500:], se[-1500:]))
        return 1
    off_moves = parse_moves(f)
    off_zs = set(round(z, 4) for lz, ty, x, y, z, e in off_moves
                 if lz is not None and z is not None and ty == "Top surface")
    print("    OK, %d moves; distinct top-surface Z values: %d" % (len(off_moves), len(off_zs)))

    # ---------------------------------------------------------------- 2. ZAA on
    print("\n--- 2. ZAA on, 0.2 mm layers, --export-3mf (GCodeProcessor pass)")
    r, f, so, se = slice_once(a.cand, dd, os.path.join(WORK, "on"),
                              ["--zaa-enabled=1", "--zaa-min-z=%s" % MIN_Z], export3mf=True)
    if r != 0 or not f:
        print("    FAIL rc=%s\n%s\n%s" % (r, so[-2500:], se[-2500:]))
        return 1
    m3 = os.path.join(WORK, "on", "out.3mf")
    print("    slice rc=0; 3mf written: %s (%s bytes)"
          % (os.path.isfile(m3), os.path.getsize(m3) if os.path.isfile(m3) else "-"))
    if not os.path.isfile(m3):
        rc = 1
        print("    !! --export-3mf produced no file (GCodeProcessor pass not proven)")
    ok, notes = report("zaa on", f)
    for n in notes:
        print("    " + n)
    rc |= 0 if ok else 1

    # ---------------------------------------------------------------- 3. ZAA + offset_layers
    print("\n--- 3. ZAA on + offset_layers on")
    r, f, so, se = slice_once(a.cand, dd, os.path.join(WORK, "on_ofs"),
                              ["--zaa-enabled=1", "--zaa-min-z=%s" % MIN_Z, "--offset-layers=1"])
    if r != 0 or not f:
        print("    FAIL rc=%s\n%s\n%s" % (r, so[-2500:], se[-2500:]))
        rc = 1
    else:
        ok, notes = report("zaa+offset", f, offset_layers=True)
        for n in notes:
            print("    " + n)
        rc |= 0 if ok else 1

    # ---------------------------------------------------------------- 4. spiral vase
    print("\n--- 4. ZAA on + spiral vase on (contouring must be skipped, no error)")
    r, f, so, se = slice_once(a.cand, dd, os.path.join(WORK, "vase"),
                              ["--zaa-enabled=1", "--spiral-mode=1",
                               "--wall-loops=1", "--top-shell-layers=0",
                               "--sparse-infill-density=0"])
    if r != 0 or not f:
        print("    FAIL rc=%s\n%s\n%s" % (r, so[-2500:], se[-2500:]))
        rc = 1
    else:
        print("    OK, slice succeeded (rc=0), %d moves" % len(parse_moves(f)))

    print("\nBAR B: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


sys.exit(main())

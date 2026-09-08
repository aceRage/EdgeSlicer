"""Bar B for ZAA constant-volumetric-flow speed scaling (zaa_speed_scaling).

For each model (the wedge and the hemisphere) at each layer height, slices with ZAA on
three ways and compares:

    off      zaa_enabled = 0                        - the control, no contouring at all
    noscale  zaa_enabled = 1, zaa_speed_scaling = 0 - the reviewed behaviour (2026-09-08 print)
    scale    zaa_enabled = 1, zaa_speed_scaling = 1 - this branch's default

and then runs zaa_speed_report.py on the `scale` G-code, with `noscale` as the Z-profile
reference: the whole claim of this change is that it moves F words and nothing else, so the Z
profile - and therefore every jitter number the review measured - must be identical.

IMPORTANT - layer-time cooling. CoolingBuffer reduces each extrusion path to ONE adjustable
speed, so a per-segment feed rate does not survive it (see the port doc, "THE BLOCKER").
This gate therefore slices with --slow-down-for-layer-cooling=0 by default, which is the
configuration in which the scaling is actually present in the file. Pass --cooling to run it
the other way and see the feature being erased.

Also reports the estimated print time from each file's own header, but does NOT assert on it:
slowing the contoured moves lengthens each layer, which makes the layer-time slowdown relax
the speed it imposes on the whole layer, so the total can legitimately fall. What is asserted
is the contoured extrusion time computed from the file itself.

usage: bar_b_speed.py --cand <exe> [--layer-heights 0.2,0.12] [--cooling]
"""
import argparse
import math
import os
import re
import shutil
import subprocess
import sys

T = os.path.dirname(os.path.abspath(__file__))
HUB = r"C:\Users\acesa\AppData\Local\Temp\snorca_hubtest"
SRC_DD = os.path.join(HUB, "dd_lan")
WORK = os.path.join(T, "bar_b_speed_work")

PRINTER = "Bambu Lab P1S 0.4 nozzle"
PROCESS = "0.20mm Standard @BBL X1C"
FILAMENT = "Generic PLA"
MIN_Z = 0.05

RE_TIME = re.compile(r"^;\s*(?:estimated printing time.*|model printing time)\s*[:=]\s*(.+?)\s*$",
                     re.I)


def hms_to_s(t):
    """'1h 2m 3s' / '2m 3s' / '3s' -> seconds."""
    s = 0
    for v, u in re.findall(r"(\d+)\s*([dhms])", t):
        s += int(v) * {"d": 86400, "h": 3600, "m": 60, "s": 1}[u]
    return s


def est_time(path):
    best = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith(";"):
                continue
            m = RE_TIME.match(line)
            if m:
                v = hms_to_s(m.group(1))
                if v and (best is None or v > best):
                    best = v
    return best


RE_MOVE = re.compile(r"^G[0123]\s")


def extrusion_time(path):
    """(contoured, non-contoured) extrusion time in seconds, from the file's own F words.

    A contoured extrusion is an extruding move that carries its own Z. This is the measure the
    speed scaling actually controls: it is free of the layer-time cooling slowdown, which
    reacts to the change and muddies the whole-print estimate.
    """
    px = py = None
    f = None
    tc = tu = 0.0
    for line in open(path, encoding="utf-8", errors="replace"):
        if not RE_MOVE.match(line):
            continue
        g = {}
        for t in line.split(";")[0].split()[1:]:
            try:
                g[t[0]] = float(t[1:])
            except (ValueError, IndexError):
                pass
        if g.get("F") is not None:
            f = g["F"]
        x, y = g.get("X", px), g.get("Y", py)
        if None not in (px, py, x, y) and g.get("E") and g["E"] > 0 and f:
            dt = math.hypot(x - px, y - py) / f * 60.0
            if g.get("Z") is not None:
                tc += dt
            else:
                tu += dt
        px, py = x, y
    return tc, tu


def slice_once(exe, dd, out, model, layer_h, extra, export3mf=False, cooling=False):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    cmd = [exe, "--slice", "0", "--allow-newer-file", "--no-thumbnails",
           "--datadir", dd, "--outputdir", out,
           "--printer-preset", PRINTER,
           "--process-preset", PROCESS,
           "--filament-presets", FILAMENT,
           "--layer-height=%s" % layer_h]
    extra = list(extra)
    if not cooling:
        # See the module docstring: with the layer-time slowdown on, CoolingBuffer collapses
        # the per-segment feed rates and the gate would measure nothing.
        extra = ["--slow-down-for-layer-cooling=0"] + extra
    if layer_h < 0.2:
        # The 0.20mm profile's initial layer height stays 0.2; keep it consistent so the only
        # thing that changes between the two runs is the layer height itself.
        extra = ["--initial-layer-print-height=%s" % layer_h] + extra
    cmd += extra
    if export3mf:
        cmd += ["--export-3mf", "out.3mf"]
    cmd += [model]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    so = p.stdout.decode("utf-8", "replace")
    se = p.stderr.decode("utf-8", "replace")
    files = sorted(os.path.join(out, f) for f in os.listdir(out) if f.endswith(".gcode"))
    return p.returncode, (files[0] if files else None), so, se


def fmt_t(s):
    if s is None:
        return "-"
    return "%dh%02dm%02ds" % (s // 3600, (s % 3600) // 60, s % 60)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cand", required=True)
    ap.add_argument("--layer-heights", default="0.2,0.12")
    ap.add_argument("--cooling", action="store_true",
                    help="leave slow_down_for_layer_cooling on; the scaling will be erased")
    ap.add_argument("--models", default=None,
                    help="comma-separated name=path pairs; defaults to the wedge and the dome")
    a = ap.parse_args()

    if a.models:
        models = [tuple(x.split("=", 1)) for x in a.models.split(",")]
    else:
        models = [("wedge", os.path.join(T, "zaa_wedge.stl")),
                  ("dome", os.path.join(T, "zaa_dome.stl"))]
    layer_hs = [float(x) for x in a.layer_heights.split(",")]

    os.makedirs(WORK, exist_ok=True)
    dd = os.path.join(WORK, "dd")
    if not os.path.isdir(dd):
        shutil.copytree(SRC_DD, dd)

    print("cand: %s" % a.cand)
    print("layer-time cooling: %s" % ("ON - the scaling will be erased, see the port doc"
                                      if a.cooling else "off"))
    rc = 0
    for mname, mpath in models:
        if not os.path.isfile(mpath):
            print("\n!! model missing: %s" % mpath)
            rc = 1
            continue
        for lh in layer_hs:
            tag = "%s_%.2f" % (mname, lh)
            print("\n" + "=" * 78)
            print("== %s at %.2f mm layers" % (mname, lh))
            print("=" * 78)

            runs = {}
            for label, extra, x3 in (
                    ("off", [], False),
                    ("noscale", ["--zaa-enabled=1", "--zaa-min-z=%s" % MIN_Z,
                                 "--zaa-speed-scaling=0"], False),
                    ("scale", ["--zaa-enabled=1", "--zaa-min-z=%s" % MIN_Z,
                               "--zaa-speed-scaling=1"], True)):
                r, f, so, se = slice_once(a.cand, dd, os.path.join(WORK, tag, label),
                                          mpath, lh, extra, export3mf=x3, cooling=a.cooling)
                if r != 0 or not f:
                    print("  %-8s FAIL rc=%s\n%s\n%s" % (label, r, so[-2000:], se[-2000:]))
                    rc = 1
                    runs[label] = None
                    continue
                runs[label] = f
                t = est_time(f)
                extra_note = ""
                if x3:
                    m3 = os.path.join(WORK, tag, label, "out.3mf")
                    extra_note = "  3mf: %s (%s bytes)" % (
                        os.path.isfile(m3),
                        os.path.getsize(m3) if os.path.isfile(m3) else "-")
                    if not os.path.isfile(m3):
                        print("  !! --export-3mf produced no file")
                        rc = 1
                print("  %-8s rc=0  est %-11s size %8d B%s"
                      % (label, fmt_t(t), os.path.getsize(f), extra_note))

            if not runs.get("scale") or not runs.get("noscale"):
                rc = 1
                continue

            t_ns, t_s = est_time(runs["noscale"]), est_time(runs["scale"])
            if t_ns and t_s:
                print("  time estimate (whole print): %s -> %s  (%+.1f %%, %+d s)"
                      % (fmt_t(t_ns), fmt_t(t_s), 100.0 * (t_s - t_ns) / t_ns, t_s - t_ns))

            # The whole-print estimate is NOT a clean measure of this change, and must not be
            # asserted on. Slowing the contoured moves lengthens each layer, which makes the
            # layer-time cooling slowdown (slow_down_for_layer_cooling) relax the speed it was
            # imposing on the WHOLE layer - so the total can legitimately come out lower even
            # though every contoured move is slower. Measured on the dome: with cooling on the
            # total fell 31 s, with --slow-down-for-layer-cooling=0 the contoured extrusion
            # time rose 9.1 -> 11.0 s (+21 %) and the non-contoured time was identical to the
            # microsecond. What is asserted instead is the contoured extrusion time computed
            # from the file itself.
            t_ns_c, t_ns_u = extrusion_time(runs["noscale"])
            t_s_c, t_s_u = extrusion_time(runs["scale"])
            print("  extrusion time from the file: contoured %.1f -> %.1f s (%+.1f %%) ; "
                  "everything else %.1f -> %.1f s"
                  % (t_ns_c, t_s_c,
                     (100.0 * (t_s_c - t_ns_c) / t_ns_c) if t_ns_c else 0.0,
                     t_ns_u, t_s_u))
            if t_ns_c and t_s_c < t_ns_c - 1e-6:
                print("  !! the contoured moves got FASTER, which the never-speed-up clamp forbids")
                rc = 1
            if abs(t_s_u - t_ns_u) > 0.05:
                print("  !! non-contoured extrusion time changed by %.2f s; the scaling must "
                      "not touch anything but contoured moves" % (t_s_u - t_ns_u))
                rc = 1

            print()
            p = subprocess.run(
                [sys.executable, "-u", os.path.join(T, "zaa_speed_report.py"), runs["scale"],
                 "--layer-height=%s" % lh, "--min-z=%s" % MIN_Z,
                 "--compare-z", runs["noscale"]],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for line in p.stdout.decode("utf-8", "replace").splitlines():
                print("  " + line)
            if p.returncode != 0:
                rc = 1

    print("\nBAR B (speed): %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


sys.exit(main())

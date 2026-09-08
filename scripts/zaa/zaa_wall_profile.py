"""Layer-to-layer smoothness of the ZAA-contoured outer wall.

Within one layer the outer wall of an axisymmetric model runs along a contour
line, so its Z barely varies (zaa_jitter.py measures that).  What the eye sees on
a printed slope is the OTHER axis: how the wall's Z offset from print_z changes
from layer to layer, i.e. how thick each wall bead actually is.  A wall whose
per-layer height swings 190 um, 232 um, 193 um, 230 um reads as uneven banding -
the "looks like fuzzy skin" complaint from the 2026-09-07 print.

Reports, per feature, the sequence of (median Z - print_z) per layer and the
distribution of the resulting effective bead height.

usage: zaa_wall_profile.py <file.gcode> [--feature "Outer wall"] [--dump]
"""
import argparse
import collections
import math
import re
import os
import statistics

RE_TYPE = re.compile(r"^;\s*FEATURE:\s*(.*)$")
RE_ZH = re.compile(r"^;\s*Z_HEIGHT:\s*([0-9.]+)")


def collect(path):
    per = collections.defaultdict(lambda: collections.defaultdict(list))
    feat = ""
    layer = None
    x = y = z = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = RE_TYPE.match(line)
        if m:
            feat = m.group(1).strip()
            continue
        m = RE_ZH.match(line)
        if m:
            layer = float(m.group(1))
            continue
        if not line.startswith(("G1 ", "G0 ")):
            continue
        nx = ny = nz = e = None
        for tok in line.split(";")[0].split()[1:]:
            c = tok[0]
            try:
                v = float(tok[1:])
            except ValueError:
                continue
            if c == "X":
                nx = v
            elif c == "Y":
                ny = v
            elif c == "Z":
                nz = v
            elif c == "E":
                e = v
        if nx is not None:
            x = nx
        if ny is not None:
            y = ny
        if nz is not None:
            z = nz
        if e is None or e <= 0 or (nx is None and ny is None):
            continue
        if layer is None or z is None:
            continue
        per[feat][layer].append(z)
    return per


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gcode")
    ap.add_argument("--feature", default="Outer wall")
    ap.add_argument("--min-moves", type=int, default=4)
    ap.add_argument("--stat", choices=["median", "min"], default="median")
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args()

    per = collect(a.gcode)[a.feature]
    layers = sorted(l for l, v in per.items() if len(v) >= a.min_moves)
    # The median is the right summary for a loop that sits at one Z (a dome); the minimum is the
    # right one for a loop with two regimes (the wedge, whose leading edge is contoured and whose
    # other three sides are not). Report whichever the caller asks for; default median.
    pick = min if a.stat == "min" else statistics.median
    prof = [(l, pick(per[l])) for l in layers]

    heights = []
    for i in range(len(prof) - 1):
        (l0, z0), (l1, z1) = prof[i], prof[i + 1]
        # only compare consecutive layers
        if abs(round((l1 - l0) / 0.2) - (l1 - l0) / 0.2) > 1e-6 or round((l1 - l0) / 0.2) != 1:
            continue
        heights.append((l1, (z1 - z0) * 1000.0))

    if a.dump:
        print("layer   dZ_um   bead_um")
        prev = None
        for l, z in prof:
            dz = (z - l) * 1000.0
            bead = "" if prev is None else "%7.1f" % ((z - prev) * 1000.0)
            print("%6.2f %7.1f %s" % (l, dz, bead))
            prev = z

    if not heights:
        print("%-30s no contoured layers" % os.path.basename(a.gcode))
        return
    hv = [h for _, h in heights]
    mu = sum(hv) / len(hv)
    sd = math.sqrt(sum((v - mu) ** 2 for v in hv) / len(hv)) if len(hv) > 1 else 0.0
    # jerk: how much the bead height changes from one layer to the next
    jerk = [abs(hv[i + 1] - hv[i]) for i in range(len(hv) - 1)]
    jm = max(jerk) if jerk else 0.0
    print("%-28s %s [%s] layers=%d  bead mean=%.1f sd=%.1f min=%.1f max=%.1f um   "
          "layer-to-layer change max=%.1f um mean=%.1f um"
          % (os.path.basename(os.path.dirname(a.gcode)), a.feature, a.stat, len(hv),
             mu, sd, min(hv), max(hv), jm, (sum(jerk) / len(jerk)) if jerk else 0.0))


if __name__ == "__main__":
    main()

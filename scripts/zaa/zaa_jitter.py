"""Measure the outer-wall Z jitter of a ZAA-contoured G-code file.

The owner's complaint after the 2026-09-07 print was that the outer wall on the
sloped region moves unevenly, "almost like fuzzy skin".  This script quantifies
that: for every contiguous run of extruding moves of one feature type inside one
layer, it reports the distribution of the Z step between NEIGHBOURING moves.

A smooth ramp has a Z step per 0.1 mm sample of  0.1 * tan(slope)  -- 25 um on the
14 deg wedge -- and, more importantly, a near-zero SECOND difference: the ramp's
slope does not change from sample to sample.  Fuzz shows up as a large second
difference and as sign reversals (up, down, up, down) along the path.

usage: zaa_jitter.py <file.gcode> [--feature "Outer wall"] [--csv out.csv]
"""
import argparse
import math
import os
import re
import sys

RE_TYPE = re.compile(r"^;\s*(?:FEATURE|TYPE):\s*(.*)$")
RE_ZH = re.compile(r"^;\s*Z_HEIGHT:\s*([0-9.]+)")


def parse(path):
    """-> list of runs; a run is dict(layer, feature, pts=[(x,y,z,e)])"""
    runs = []
    cur = None
    feature = ""
    layer = None
    x = y = z = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = RE_TYPE.match(line)
        if m:
            feature = m.group(1).strip()
            cur = None
            continue
        m = RE_ZH.match(line)
        if m:
            layer = float(m.group(1))
            cur = None
            continue
        if not (line.startswith("G1 ") or line.startswith("G0 ")):
            continue
        nx = ny = nz = None
        e = None
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
        extruding = e is not None and e > 0 and (nx is not None or ny is not None)
        if not extruding:
            cur = None
            continue
        if cur is None:
            cur = {"layer": layer, "feature": feature, "pts": []}
            runs.append(cur)
        cur["pts"].append((x, y, z, e))
    return runs


def stats(vals):
    if not vals:
        return dict(n=0, mean=0.0, sd=0.0, mx=0.0, p99=0.0)
    n = len(vals)
    mean = sum(vals) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in vals) / n) if n > 1 else 0.0
    s = sorted(vals)
    return dict(n=n, mean=mean, sd=sd, mx=s[-1], p99=s[min(n - 1, int(0.99 * n))])


def analyse(runs, feature_filter=None, min_pts=6):
    """Per-run Z-step statistics, in microns."""
    d1 = []          # |z[i+1] - z[i]|, the step between neighbouring samples
    d2 = []          # |second difference|, the curvature of the Z profile
    reversals = 0
    steps_total = 0
    contoured_runs = 0
    per_run = []
    for r in runs:
        if feature_filter and r["feature"] != feature_filter:
            continue
        pts = r["pts"]
        if len(pts) < min_pts:
            continue
        zs = [p[2] for p in pts if p[2] is not None]
        if len(zs) < min_pts:
            continue
        if max(zs) - min(zs) < 1e-9:
            continue  # not contoured
        contoured_runs += 1
        dz = [(zs[i + 1] - zs[i]) * 1000.0 for i in range(len(zs) - 1)]
        dd = [abs(dz[i + 1] - dz[i]) for i in range(len(dz) - 1)]
        rev = sum(1 for i in range(len(dz) - 1)
                  if dz[i] * dz[i + 1] < 0 and abs(dz[i]) > 0.5 and abs(dz[i + 1]) > 0.5)
        d1 += [abs(v) for v in dz]
        d2 += dd
        reversals += rev
        steps_total += len(dz)
        per_run.append(dict(layer=r["layer"], feature=r["feature"], n=len(zs),
                            span=(max(zs) - min(zs)) * 1000.0,
                            d1=stats([abs(v) for v in dz]), d2=stats(dd), rev=rev))
    return dict(runs=contoured_runs, d1=stats(d1), d2=stats(d2),
                reversals=reversals, steps=steps_total, per_run=per_run)


def report(name, res):
    d1, d2 = res["d1"], res["d2"]
    print("%-22s runs=%-5d steps=%-7d  |dZ| mean=%6.2f sd=%6.2f p99=%7.2f max=%8.2f um"
          % (name, res["runs"], res["steps"], d1["mean"], d1["sd"], d1["p99"], d1["mx"]))
    print("%-22s                        |d2Z| mean=%6.2f sd=%6.2f p99=%7.2f max=%8.2f um   reversals=%d (%.2f%%)"
          % ("", d2["mean"], d2["sd"], d2["p99"], d2["mx"], res["reversals"],
             100.0 * res["reversals"] / max(1, res["steps"])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gcode")
    ap.add_argument("--feature", action="append", default=None)
    ap.add_argument("--worst", type=int, default=0)
    a = ap.parse_args()

    runs = parse(a.gcode)
    feats = a.feature or ["Outer wall", "Inner wall", "Top surface"]
    print("== %s" % os.path.basename(a.gcode))
    allres = {}
    for f in feats:
        res = analyse(runs, f)
        allres[f] = res
        if res["runs"]:
            report(f, res)
        else:
            print("%-22s no contoured runs" % f)
    if a.worst:
        for f in feats:
            pr = sorted(allres[f]["per_run"], key=lambda r: -r["d2"]["mx"])[:a.worst]
            for r in pr:
                print("   worst %s layer=%.3f n=%d span=%.1fum d1max=%.1f d2max=%.1f rev=%d"
                      % (f, r["layer"] or -1, r["n"], r["span"], r["d1"]["mx"], r["d2"]["mx"], r["rev"]))
    return allres


if __name__ == "__main__":
    main()

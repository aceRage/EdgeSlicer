"""Bar B analyzer for ZAA constant-flow speed scaling (zaa_speed_scaling).

The rule under test, per contoured extrusion segment:

    F_seg = clamp( F_role * h_seg / H , floor , F_role )   then capped by max_volumetric_speed

The emitter has already scaled the segment's E by h_seg / H - the bead really is that much
thinner. Scaling F by the same ratio returns the extruder's melt rate, and the time the Z axis
gets for each step, to what the role was tuned for at the nominal layer height H. A half-height
bead runs at half speed; a bead at the nominal height is untouched.

HOW IT IS MEASURED, without needing to know any role's configured speed
----------------------------------------------------------------------
Within ONE contoured path the role's F is a single constant, so the rule is equivalent to

    F_seg / h_seg  =  F_role / H  =  the same value for every segment of that path

and that is a self-contained invariant of the emitted G-code. It needs no reference slice, no
config, and no assumption about which feature maps to which speed setting. Segments where a
clamp legitimately binds are excluded and counted rather than silently passed:

  * h_seg > H              - the never-speed-up clamp holds F at F_role, so F/h is deliberately
                             low there;
  * F at the 10 mm/s floor - the rule cannot slow down any further.

Two earlier versions of this script tried to infer F_role - once from the maximum F in the
scaled file, once from a reference slice - and compare absolute flows. Both fail. A path whose
contour is flat-but-not-nominal (a whole wall at a constant 0.178 mm, say) is scaled uniformly,
so the largest F in the scaled file is already a scaled value and the inferred role speed came
out 11 % low; and the largest F in a reference slice is a travel or first-layer speed, not the
role's. The within-path ratio has neither problem.

Checks:
  1. F_seg / h_seg is constant within every contoured path, to within --tol (default 3 %),
     excluding the clamped segments, which are counted;
  2. no deviation exceeds the 5 % hysteresis band - a segment may keep an F that was set for a
     height up to 5 % away, and that is the only source of residual, so anything beyond it is a
     real failure;
  3. no F below the 10 mm/s floor;
  4. F churn: how many contoured segments carry an explicit F word, and whether any was emitted
     while still inside the +-5 % hysteresis band;
  5. with --compare-z, the Z profile is identical to a reference run (the same slice with
     scaling off) - the whole claim of this change is that it moves F words and nothing else, so
     every jitter and bead-height number the 2026-09-07 review measured must be untouched.

usage: zaa_speed_report.py <gcode> [--layer-height 0.2] [--min-z 0.05] [--tol 0.03]
                                   [--offset-layers] [--compare-z <other.gcode>]
"""
import argparse
import collections
import math
import re
import sys

RE_G1 = re.compile(r"^G[01]\s")
# G2/G3 move the head too. They are never contoured (arc fitting is disabled on a contoured
# path), but the tool position after one is NOT the previous G1's endpoint, so a G1 that follows
# an arc has no usable segment length and must be dropped rather than measured against a stale
# position. An earlier version of this script did exactly that and reported a phantom 11 %.
RE_ARC = re.compile(r"^G[23]\s")
RE_FEATURE = re.compile(r"^;\s*FEATURE:\s*(.*)$")
RE_ZH = re.compile(r"^;\s*Z_HEIGHT:\s*([0-9.]+)")

# The floor the emitter enforces, mm/min (ZAA_MIN_SPEED_MM_S = 10 mm/s in ContourZ.hpp).
FLOOR_MM_MIN = 10.0 * 60.0
# ZAA_SPEED_HYSTERESIS in ContourZ.hpp.
HYST = 0.05


def parse(path):
    """Every G0/G1/G2/G3 with the layer print_z, feature, segment length, E, and the F in force."""
    out = []
    feat = ""
    lz = None
    px = py = None
    zc = None
    f_cur = None
    after_arc = False
    for line in open(path, encoding="utf-8", errors="replace"):
        m = RE_FEATURE.match(line)
        if m:
            feat = m.group(1).strip()
            continue
        m = RE_ZH.match(line)
        if m:
            lz = float(m.group(1))
            continue
        if RE_ARC.match(line):
            g = {}
            for tok in line.split(";")[0].split()[1:]:
                try:
                    g[tok[0]] = float(tok[1:])
                except (ValueError, IndexError):
                    pass
            px, py = g.get("X", px), g.get("Y", py)
            if g.get("F") is not None:
                f_cur = g["F"]
            after_arc = True
            out.append(dict(feat=feat, lz=lz, x=px, y=py, z=None, e=None, dist=None,
                            zprev=zc, f=f_cur, f_word=None, arc=True))
            continue
        if not RE_G1.match(line):
            continue
        g = {}
        for tok in line.split(";")[0].split()[1:]:
            try:
                g[tok[0]] = float(tok[1:])
            except (ValueError, IndexError):
                pass
        x, y = g.get("X", px), g.get("Y", py)
        d = None
        if px is not None and py is not None and x is not None and y is not None and not after_arc:
            d = math.hypot(x - px, y - py)
        zprev = zc
        if g.get("Z") is not None:
            zc = g["Z"]
        f_word = g.get("F")
        if f_word is not None:
            f_cur = f_word
        out.append(dict(feat=feat, lz=lz, x=x, y=y, z=g.get("Z"), e=g.get("E"), dist=d,
                        zprev=zprev, f=f_cur, f_word=f_word, arc=False))
        after_arc = False
        px, py = x, y
    return out


def is_contoured(m):
    """An extruding G1 that carries its own Z, with a usable segment length and an F in force."""
    return (not m["arc"] and m["e"] is not None and m["e"] > 0 and m["z"] is not None
            and m["lz"] is not None and m["dist"] and m["dist"] > 0.05
            and m["zprev"] is not None and m["f"])


def local_height(m, H, MZ, bases):
    """The segment's local layer height: H + the trapezoid mean of its endpoint deltas."""
    dz_raw = 0.5 * ((m["z"] - m["lz"]) + (m["zprev"] - m["lz"]))
    best = None
    for b in bases:
        h = H + dz_raw - b
        if MZ - 1e-6 <= h <= H + MZ + 1e-6:
            if best is None or abs(h - H) < abs(best - H):
                best = h
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gcode")
    ap.add_argument("--layer-height", type=float, default=0.2)
    ap.add_argument("--min-z", type=float, default=0.05)
    ap.add_argument("--tol", type=float, default=0.03)
    ap.add_argument("--offset-layers", action="store_true")
    ap.add_argument("--compare-z", default=None,
                    help="a reference G-code (same slice, scaling off) whose Z profile must match")
    a = ap.parse_args()
    H, MZ, TOL = a.layer_height, a.min_z, a.tol
    bases = [0.0, 0.5 * H] if a.offset_layers else [0.0]

    moves = parse(a.gcode)

    # ---------------------------------------------------------------- group into contoured paths
    paths = []
    cur = None
    for m in moves:
        if is_contoured(m):
            h = local_height(m, H, MZ, bases)
            if h is None:
                cur = None
                continue
            if cur is None:
                cur = dict(feat=m["feat"], segs=[])
                paths.append(cur)
            cur["segs"].append(dict(h=h, f=m["f"], f_word=m["f_word"], epm=m["e"] / m["dist"]))
        else:
            cur = None

    n_seg = sum(len(p["segs"]) for p in paths)
    print("file: %s" % a.gcode)
    print("moves: %d ; contoured paths: %d ; contoured segments: %d"
          % (len(moves), len(paths), n_seg))
    if not n_seg:
        print("RESULT: FAIL - nothing was contoured")
        return 1

    ok = True
    n_f_words = sum(1 for p in paths for s in p["segs"] if s["f_word"] is not None)
    print("contoured segments carrying an explicit F word: %d of %d (%.1f %%)"
          % (n_f_words, n_seg, 100.0 * n_f_words / n_seg))

    # ---------------------------------------------------------------- 1-3. the invariant
    agg = collections.defaultdict(lambda: dict(n=0, tot=0.0, worst=0.0, bad=0, over_h=0,
                                               clamped=0, floored=0, below_floor=0,
                                               hlo=9e9, hhi=-9e9, flo=9e9, fhi=-9e9))
    for p in paths:
        segs = p["segs"]
        a_ = agg[p["feat"]]
        a_["clamped"] += sum(1 for s in segs if s["h"] > H + 1e-9)
        a_["floored"] += sum(1 for s in segs if abs(s["f"] - FLOOR_MM_MIN) <= 1e-6)
        a_["below_floor"] += sum(1 for s in segs if s["f"] < FLOOR_MM_MIN - 1e-6)
        for s in segs:
            a_["hlo"] = min(a_["hlo"], s["h"]); a_["hhi"] = max(a_["hhi"], s["h"])
            a_["flo"] = min(a_["flo"], s["f"]); a_["fhi"] = max(a_["fhi"], s["f"])
        # Unclamped segments only.
        free = [s for s in segs if s["h"] <= H + 1e-9 and s["f"] > FLOOR_MM_MIN + 1e-6]
        if len(free) < 3:
            continue
        ratios = sorted(s["f"] / s["h"] for s in free)
        med = ratios[len(ratios) // 2]
        if med <= 0:
            continue
        for s in free:
            dev = abs((s["f"] / s["h"]) / med - 1.0)
            a_["n"] += 1
            a_["tot"] += dev
            a_["worst"] = max(a_["worst"], dev)
            if dev > TOL:
                a_["bad"] += 1
            if dev > HYST + 1e-6:
                a_["over_h"] += 1

    print()
    print("Within-path invariant  F_seg / h_seg = F_role / H  (constant along each contoured path):")
    total_n = total_bad = total_overh = total_clamped = total_floored = total_belowfloor = 0
    for feat in sorted(agg):
        a_ = agg[feat]
        if not a_["n"]:
            print("  %-16s no unclamped segments to measure (h %.3f..%.3f, clamped %d, floored %d)"
                  % (feat, a_["hlo"], a_["hhi"], a_["clamped"], a_["floored"]))
            continue
        print("  %-16s n=%-6d h_seg %.3f..%.3f mm   F %.0f..%.0f mm/min"
              % (feat, a_["n"], a_["hlo"], a_["hhi"], a_["flo"], a_["fhi"]))
        print("  %-16s   deviation from the path's own F/h: mean %.3f %% max %.3f %% ; "
              "outside %.0f %%: %d ; outside the %.0f %% hysteresis band: %d"
              % ("", 100.0 * a_["tot"] / a_["n"], 100.0 * a_["worst"], TOL * 100.0,
                 a_["bad"], HYST * 100.0, a_["over_h"]))
        print("  %-16s   at the never-speed-up clamp (h > H): %d ; at the floor: %d ; "
              "BELOW the floor: %d"
              % ("", a_["clamped"], a_["floored"], a_["below_floor"]))
        total_n += a_["n"]; total_bad += a_["bad"]; total_overh += a_["over_h"]
        total_clamped += a_["clamped"]; total_floored += a_["floored"]
        total_belowfloor += a_["below_floor"]
        if a_["over_h"] or a_["below_floor"]:
            ok = False

    print()
    print("TOTAL: %d unclamped segments; %d outside %.0f %%, of which %d outside the %.0f %% "
          "hysteresis band (which must be 0)."
          % (total_n, total_bad, TOL * 100.0, total_overh, HYST * 100.0))
    print("       %d segments held at the never-speed-up clamp, %d at the floor, %d below it "
          "(must be 0)." % (total_clamped, total_floored, total_belowfloor))
    if total_n == 0:
        ok = False
        print("!! nothing could be measured")

    # ---------------------------------------------------------------- 4. hysteresis / F churn
    churn_bad = 0
    for p in paths:
        h_ref = 0.0
        for s in p["segs"]:
            if s["f_word"] is not None:
                if h_ref > 0.0 and abs(s["h"] - h_ref) <= HYST * h_ref:
                    churn_bad += 1
                h_ref = s["h"]
    print()
    print("hysteresis: F words emitted while still inside the +-%.0f %% band: %d"
          % (HYST * 100.0, churn_bad))
    print("            (a dynamic-speed change legitimately forces a re-emit; this walk cannot "
          "always see one, so a small count is not a failure)")

    # ---------------------------------------------------------------- 5. the Z profile
    if a.compare_z:
        ref = parse(a.compare_z)

        def zprof(ms):
            return [round(m["z"], 4) for m in ms
                    if not m["arc"] and m["e"] is not None and m["e"] > 0 and m["z"] is not None]

        za, zb = zprof(moves), zprof(ref)
        same = (za == zb)
        print()
        print("Z profile vs %s:" % a.compare_z)
        print("  %d vs %d contoured Z values, identical: %s" % (len(za), len(zb), same))
        if not same:
            ok = False
            n = min(len(za), len(zb))
            first = next((i for i in range(n) if za[i] != zb[i]), n)
            print("  !! first difference at index %d: %s vs %s"
                  % (first, za[first] if first < len(za) else "-",
                     zb[first] if first < len(zb) else "-"))

    print("\nRESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


sys.exit(main())

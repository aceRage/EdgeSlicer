"""Bar B analyzer for the ZAA port - the checks, split out so they can be re-run on
already-sliced G-code.

The key distinction: a move that carries an EXPLICIT Z on an extruding G1 is one the
ZAA emitter produced (extrude_to_xyz). Everything else went through extrude_to_xy at
the layer's own Z. Travels are ignored entirely - an earlier version of this script
let a travel's Z leak into the following extrusions and passed vacuously.

usage: bar_b_report.py <gcode> [--layer-height 0.2] [--min-z 0.05] [--offset-layers]
"""
import argparse
import collections
import math
import re
import sys

RE_G1 = re.compile(r"^G[01]\s")
RE_FEATURE = re.compile(r"^;\s*FEATURE:\s*(.*)$")
RE_ZH = re.compile(r"^;\s*Z_HEIGHT:\s*([0-9.]+)")


def parse(path):
    """-> list of dicts for every G0/G1, with the layer's print_z and feature."""
    out = []
    feat = ""
    lz = None
    px = py = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = RE_FEATURE.match(line)
        if m:
            feat = m.group(1).strip()
            continue
        m = RE_ZH.match(line)
        if m:
            lz = float(m.group(1))
            continue
        if not RE_G1.match(line):
            continue
        body = line.split(";")[0]
        g = {}
        for tok in body.split()[1:]:
            try:
                g[tok[0]] = float(tok[1:])
            except ValueError:
                pass
        x, y = g.get("X", px), g.get("Y", py)
        d = None
        if px is not None and py is not None and x is not None and y is not None:
            d = math.hypot(x - px, y - py)
        out.append(dict(feat=feat, lz=lz, x=x, y=y, z=g.get("Z"), e=g.get("E"), dist=d))
        px, py = x, y
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gcode")
    ap.add_argument("--layer-height", type=float, default=0.2)
    ap.add_argument("--min-z", type=float, default=0.05)
    ap.add_argument("--offset-layers", action="store_true")
    a = ap.parse_args()
    H, MZ = a.layer_height, a.min_z

    moves = parse(a.gcode)
    # Contoured extrusions: extruding G1 that carries its own Z.
    con = [m for m in moves
           if m["e"] is not None and m["e"] > 0 and m["z"] is not None and m["lz"] is not None]

    print("file: %s" % a.gcode)
    print("moves: %d, extrusions carrying their own Z (i.e. contoured): %d" % (len(moves), len(con)))
    if not con:
        print("RESULT: FAIL - nothing was contoured")
        return 1

    ok = True

    # ---- 1. Z varies within a layer, and per feature.
    per_layer = collections.defaultdict(set)
    per_feat = collections.Counter()
    for m in con:
        per_layer[round(m["lz"], 4)].add(round(m["z"], 4))
        per_feat[m["feat"]] += 1
    varying = sum(1 for zs in per_layer.values() if len(zs) > 1)
    print("layers with contoured extrusions: %d ; of those, layers carrying more than one Z: %d"
          % (len(per_layer), varying))
    print("contoured extrusions by feature: %s" % dict(per_feat))
    if varying == 0:
        ok = False
        print("!! no layer varies Z across its contoured extrusions")

    # ---- 2. The clamp.
    #
    # What the code guarantees for a contoured path whose own base is
    #     base = print_z + z_offset * H,  z_offset in {0, 0.5}
    # is EITHER a contoured sample, whose absolute Z lands in [lo + min_z, print_z + min_z]
    # whatever the base was, OR an uncontoured sample, which stays exactly on its base.
    bases = [0.0, 0.5 * H] if a.offset_layers else [0.0]
    bad = []
    for m in con:
        lz, z = m["lz"], m["z"]
        in_band = (lz - H + MZ - 1e-6) <= z <= (lz + MZ + 1e-6)
        on_base = any(abs(z - (lz + b)) < 1e-6 for b in bases)
        if not (in_band or on_base):
            bad.append(m)
    on_base_n = sum(1 for m in con
                    if any(abs(m["z"] - (m["lz"] + b)) < 1e-6 for b in bases)
                    and not ((m["lz"] - H + MZ - 1e-6) <= m["z"] <= (m["lz"] + MZ + 1e-6)))
    print("contoured extrusions inside the absolute band [lo+min_z, print_z+min_z]: %d" % (len(con) - on_base_n))
    print("contoured extrusions resting on their own uncontoured base: %d" % on_base_n)
    print("contoured extrusions outside both: %d" % len(bad))
    if bad:
        ok = False
        print("!! the clamp was violated")
        for m in bad[:5]:
            print("   !! %s lz=%s z=%s" % (m["feat"], m["lz"], m["z"]))

    # ---- 3. E per mm must track the local layer height, with ONE height term.
    #
    # The emitter multiplies e_per_mm by (H + d) / H, where d is measured from the path's own
    # base. So for a fixed feature and base, E/mm divided by that ratio must be constant. We do
    # not know each path's base, so we try both and report the one that fits - and the base that
    # fits IS the claim: a raised odd wall's flow follows the summed local height, not the
    # offset-layers bonding factor times the ZAA height.
    print()
    print("E per mm / ((H + d) / H) must be constant, per feature; d measured from the path base:")
    samples = collections.defaultdict(list)
    for m in con:
        if m["dist"] and m["dist"] > 0.05:
            samples[m["feat"]].append((m["z"] - m["lz"], m["e"] / m["dist"]))
    checked = 0
    for feat in sorted(samples):
        ss = samples[feat]
        if len(ss) < 20:
            continue
        if len({round(dz, 4) for dz, _ in ss}) < 2:
            print("  %-22s n=%-5d only one Z offset, nothing to compare" % (feat, len(ss)))
            continue
        best = None
        for b in bases:
            rs = []
            for dz, epm in ss:
                ratio = (H + dz - b) / H
                if ratio > 1e-6:
                    rs.append(epm / ratio)
            if len(rs) < len(ss):
                continue
            mean = sum(rs) / len(rs)
            if mean <= 0:
                continue
            cv = (sum((r - mean) ** 2 for r in rs) / len(rs)) ** 0.5 / mean
            if best is None or cv < best[0]:
                best = (cv, b, mean)
        if best is None:
            print("  %-22s n=%-5d could not be evaluated" % (feat, len(ss)))
            continue
        cv, b, mean = best
        good = cv < 0.03
        checked += 1
        print("  %-22s n=%-5d base = print_z + %.2f*H   e_per_mm = %.5f   spread %.3f%%   %s"
              % (feat, len(ss), b / H, mean, cv * 100.0, "ok" if good else "MISMATCH"))
        if not good:
            ok = False
    if checked == 0:
        ok = False
        print("  !! no feature had enough varying-height samples to check the flow")

    # ---- 4. offset_layers: an odd wall's base is print_z + 0.5h; contoured walls still capped.
    if a.offset_layers:
        walls = [m for m in moves
                 if m["e"] is not None and m["e"] > 0 and m["lz"] is not None
                 and "wall" in m["feat"].lower()]
        raised = [m for m in walls if m["z"] is not None
                  and abs(m["z"] - (m["lz"] + 0.5 * H)) < 1e-6]
        print("\noffset_layers: wall extrusions %d ; sitting exactly on the raised base "
              "print_z + 0.5h (uncontoured odd walls): %d" % (len(walls), len(raised)))
        bad = [m for m in walls if m["z"] is not None and m["z"] > m["lz"] + MZ + 1e-6
               and abs(m["z"] - (m["lz"] + 0.5 * H)) >= 1e-6]
        print("wall extrusions above print_z + zaa_min_z that are NOT on the plain raised base: %d"
              % len(bad))
        if bad:
            ok = False
            for m in bad[:5]:
                print("   !! %s lz=%s z=%s" % (m["feat"], m["lz"], m["z"]))

    print("\nRESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


sys.exit(main())

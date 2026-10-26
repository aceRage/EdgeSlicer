#!/usr/bin/env python3
"""Showcase golden-image harness: compare two renders.

    python -I compare.py <golden.png> <candidate.png> [--max-delta 2] [--mean-delta 0.25]
                         [--max-delta-e 4] [--mean-delta-e 1] [--alpha-exact] [--json out.json]

Prints one line of numbers and exits 0 when the candidate is within tolerance, 1 when not,
2 when an image is missing or the sizes differ.

- max / mean: the largest and the mean absolute difference over the R, G, B (and A) channels, in
  8-bit steps (0..255).
- dE: CIE76 colour difference (Lab, D65) per pixel: its mean and its maximum.
- over2: how many pixels differ by more than 2 steps in any channel.
- --alpha-exact: the alpha channel must be byte-identical wherever the golden is fully transparent
  (where there is no object or shadow: the transparent export's "nothing there").
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image


def srgb_to_lab(rgb):
    c = rgb / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124564, 0.3575761, 0.1804375],
                  [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = lin @ m.T
    xyz /= np.array([0.95047, 1.0, 1.08883])
    eps = 216.0 / 24389.0
    k = 24389.0 / 27.0
    f = np.where(xyz > eps, np.cbrt(xyz), (k * xyz + 16.0) / 116.0)
    lab = np.empty_like(f)
    lab[..., 0] = 116.0 * f[..., 1] - 16.0
    lab[..., 1] = 500.0 * (f[..., 0] - f[..., 1])
    lab[..., 2] = 200.0 * (f[..., 1] - f[..., 2])
    return lab


def load(path):
    im = Image.open(path)
    im.load()
    return np.asarray(im.convert("RGBA")).astype(np.float64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("candidate")
    ap.add_argument("--max-delta", type=float, default=2.0)
    ap.add_argument("--mean-delta", type=float, default=0.25)
    ap.add_argument("--max-delta-e", type=float, default=4.0)
    ap.add_argument("--mean-delta-e", type=float, default=1.0)
    ap.add_argument("--alpha-exact", action="store_true")
    ap.add_argument("--diff", help="write an amplified difference image here")
    ap.add_argument("--json", help="write the numbers here")
    a = ap.parse_args()
    try:
        g = load(a.golden)
        c = load(a.candidate)
    except Exception as ex:  # noqa: BLE001 - any read failure is a missing image
        print("ERROR could not read: %s" % ex)
        return 2
    if g.shape != c.shape:
        print("ERROR size differs: %s vs %s" % (g.shape, c.shape))
        return 2
    d = np.abs(g - c)
    max_delta = float(d.max())
    mean_delta = float(d.mean())
    over2 = int((d.max(axis=2) > 2.0).sum())
    de = np.linalg.norm(srgb_to_lab(g[..., :3]) - srgb_to_lab(c[..., :3]), axis=2)
    # Where both are fully transparent the colour carries nothing.
    visible = (g[..., 3] > 0) | (c[..., 3] > 0)
    de = np.where(visible, de, 0.0)
    max_de = float(de.max())
    mean_de = float(de[visible].mean()) if visible.any() else 0.0
    alpha_ok = True
    if a.alpha_exact:
        empty = g[..., 3] == 0
        alpha_ok = bool((g[..., 3][empty] == c[..., 3][empty]).all())
    ok = (max_delta <= a.max_delta and mean_delta <= a.mean_delta and max_de <= a.max_delta_e and
          mean_de <= a.mean_delta_e and alpha_ok)
    res = {
        "golden": a.golden, "candidate": a.candidate, "width": int(g.shape[1]), "height": int(g.shape[0]),
        "max_delta": max_delta, "mean_delta": mean_delta, "pixels_over_2": over2,
        "max_delta_e": max_de, "mean_delta_e": mean_de, "alpha_exact": alpha_ok if a.alpha_exact else None,
        "ok": ok,
    }
    if a.diff:
        amp = np.clip(d[..., :3].max(axis=2) * 32.0, 0, 255).astype(np.uint8)
        Image.fromarray(amp, "L").save(a.diff)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=2)
    print("%s max=%g mean=%.4f over2=%d dE_max=%.3f dE_mean=%.4f%s" % (
        "PASS" if ok else "FAIL", max_delta, mean_delta, over2, max_de, mean_de,
        "" if not a.alpha_exact else " alpha_exact=%s" % alpha_ok))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

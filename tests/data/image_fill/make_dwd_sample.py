#!/usr/bin/env python3
"""A textured sample for dual wall dithering on a textured model (P4).

  dwd_p4_vase.glb   a closed, lathe-turned vase shape, 32 mm across and 45 mm tall, with a
                    512 x 512 texture wrapped once around it: six colour panels (cyan, blue,
                    magenta, red, yellow, green - the three filaments and their pair mixes),
                    fading to white towards the top, with white wave lines across them.

Made for a cyan / magenta / yellow / white filament set. Written with make_glb.py's own PNG and
GLB writers, so nothing about it comes from the reader it is used to test.

    python make_dwd_sample.py [output folder]
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_glb import build_glb, png_rgb  # noqa: E402

SEGMENTS = 96
RINGS = 45
HEIGHT = 45.0
TEX = 512

PANELS = [(0, 153, 230), (40, 60, 200), (230, 26, 128), (225, 40, 40), (255, 230, 26), (40, 170, 80)]


def radius(t):
    """Radius at height fraction t: a gentle belly, a neck, a small flare. No steep overhang."""
    return 12.5 + 3.5 * math.sin(math.pi * (t * 1.15 + 0.05)) - 2.0 * t * t


def texel(x, y):
    u = x / float(TEX)
    v = 1.0 - y / float(TEX - 1)          # 0 at the foot, 1 at the top
    panel = PANELS[int(u * len(PANELS)) % len(PANELS)]
    # White wave lines, three of them, each about 1.2 mm high on the part.
    for k in range(3):
        centre = 0.2 + 0.25 * k + 0.035 * math.sin(2.0 * math.pi * (u * 6.0 + k / 3.0))
        if abs(v - centre) < 0.014:
            return 255, 255, 255
    # Full colour up to 55 % of the height, then fade to white.
    fade = 0.0 if v < 0.55 else min(1.0, (v - 0.55) / 0.4)
    return tuple(int(round(c + (255 - c) * fade)) for c in panel)


def vase():
    positions, normals, uvs, indices = [], [], [], []
    # Side: (SEGMENTS + 1) x (RINGS + 1) vertices; the seam column is doubled so u runs 0..1.
    for j in range(RINGS + 1):
        t = j / float(RINGS)
        r = radius(t)
        for i in range(SEGMENTS + 1):
            a = 2.0 * math.pi * i / SEGMENTS
            positions.append((r * math.cos(a), HEIGHT * t, r * math.sin(a)))
            normals.append((math.cos(a), 0.0, math.sin(a)))
            uvs.append((i / float(SEGMENTS), 1.0 - t))      # glTF: v runs down from the top
    row = SEGMENTS + 1
    for j in range(RINGS):
        for i in range(SEGMENTS):
            p00, p10 = j * row + i, j * row + i + 1
            p01, p11 = (j + 1) * row + i, (j + 1) * row + i + 1
            indices += [p00, p01, p11, p00, p11, p10]       # outward
    # Caps: the foot takes the bottom row of the texture's colour, the top is white.
    for top in (False, True):
        t = 1.0 if top else 0.0
        r = radius(t)
        v = 0.002 if top else 0.998
        centre = len(positions)
        positions.append((0.0, HEIGHT * t, 0.0))
        normals.append((0.0, 1.0 if top else -1.0, 0.0))
        uvs.append((0.5, v))
        for i in range(SEGMENTS + 1):
            a = 2.0 * math.pi * i / SEGMENTS
            positions.append((r * math.cos(a), HEIGHT * t, r * math.sin(a)))
            normals.append((0.0, 1.0 if top else -1.0, 0.0))
            uvs.append((i / float(SEGMENTS), v))
        for i in range(SEGMENTS):
            a, b = centre + 1 + i, centre + 2 + i
            indices += [centre, b, a] if top else [centre, a, b]
    return positions, normals, uvs, indices


def signed_volume(positions, indices):
    total = 0.0
    for k in range(0, len(indices), 3):
        a, b, c = positions[indices[k]], positions[indices[k + 1]], positions[indices[k + 2]]
        total += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                  a[2] * (b[0] * c[1] - b[1] * c[0]))
    return total / 6.0


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    p, n, uv, idx = vase()
    volume = signed_volume(p, idx)
    assert volume > 0.0, "the solid must be wound outward"
    glb = build_glb(p, n, uv, idx, png_rgb(TEX, TEX, texel), "dwd p4 vase")
    path = os.path.join(out_dir, "dwd_p4_vase.glb")
    open(path, "wb").write(glb)
    print("%s  %d bytes, %d triangles, volume %.0f mm3" % (path, len(glb), len(idx) // 3, volume))


if __name__ == "__main__":
    main()

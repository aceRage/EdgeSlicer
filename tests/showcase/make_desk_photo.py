#!/usr/bin/env python3
"""Showcase golden-image harness: a stand-in "photo" of a desk for the photo backplate fixture.

Pure Python (no image libraries), deterministic. A pinhole camera 42 mm above a wooden desk looks
330 mm ahead at a pitch of 7.25 degrees through a 50 mm (35 mm equivalent) lens, 16:9; a pale wall
stands 600 mm behind that point with a soft window light on the left. The far desk and the wall carry
only soft detail, as a shallow photo would. fixtures/backplate_desk.json matches its camera:
plate_horizon 0.80, plate_focal 50, the eye 42 mm above the floor and 330 mm in front of the model.

    python -I make_desk_photo.py <out.png> [width height]
"""
import math
import struct
import sys
import zlib

HALF_DIAGONAL = 21.633307652783937   # full frame, mm


def write_png(path, w, h, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def srgb(v):
    v = min(max(v, 0.0), 1.0)
    v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1.0 / 2.4) - 0.055
    return int(round(v * 255.0))


def hash01(n):
    # A small integer hash to [0, 1) for the plank tones.
    n = (n * 374761393 + 668265263) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65536.0


def desk(x, y, dist):
    # Planks run left to right (along x), 110 mm deep, each a little different.
    plank = int(math.floor(y / 110.0))
    tone = 0.85 + 0.3 * hash01(plank + 7)
    # Grain: long wavy lines along x, a slower figure, both fading with distance like a lens out of focus.
    soft = min(1.0, 260.0 / max(dist, 1.0))
    ph = 6.2831853 * hash01(plank * 3 + 1)
    period = 2.2 + 1.6 * hash01(plank * 5 + 2)
    yy = y + 9.0 * math.sin(x * 0.0041 + ph) + 3.0 * math.sin(x * 0.019 + 2.0 * ph)
    g = math.sin(6.2831853 * yy / period)
    g = g * g * g   # thin dark lines, wide light ones
    figure = math.sin(x * 0.0023 + ph) * math.sin(yy * 0.21 + ph)
    grain = 1.0 + soft * (0.06 * g + 0.07 * figure)
    seam = 1.0
    fy = y - plank * 110.0
    if fy < 1.2 or fy > 108.8:
        seam = 1.0 - 0.35 * soft
    base = (0.36 * tone, 0.215 * tone, 0.12 * tone)
    # The window light from the back left, falling off towards the front right.
    light = 0.55 + 0.45 * math.exp(-((x + 260.0) ** 2) / (2.0 * 380.0 ** 2)) * min(1.0, max(0.25, (y + 200.0) / 900.0))
    k = grain * seam * light
    return (base[0] * k, base[1] * k, base[2] * k)


def wall(x, z):
    # A pale warm wall, brighter towards the window on the left and the top.
    w = math.exp(-((x + 420.0) ** 2) / (2.0 * 320.0 ** 2)) * math.exp(-((z - 260.0) ** 2) / (2.0 * 220.0 ** 2))
    b = 0.42 + 0.08 * min(1.0, z / 400.0) + 0.75 * w
    return (0.80 * b, 0.76 * b, 0.70 * b)


def main(argv):
    out = argv[1] if len(argv) > 1 else "desk_photo.png"
    w = int(argv[2]) if len(argv) > 2 else 960
    h = int(argv[3]) if len(argv) > 3 else 540
    aspect = w / h
    tan_h = (HALF_DIAGONAL / math.sqrt(aspect * aspect + 1.0)) / 50.0
    cam_z, ahead, wall_y = 42.0, 330.0, 330.0 + 600.0
    p = math.atan2(cam_z, ahead)
    fwd = (0.0, math.cos(p), -math.sin(p))
    up = (0.0, math.sin(p), math.cos(p))
    def shade(sx, sy):
        dx, dy, dz = sx, fwd[1] + sy * up[1], fwd[2] + sy * up[2]
        if dz < 0.0:
            t = cam_z / -dz
            y = dy * t
            if y < wall_y:
                return desk(dx * t, y, t * math.sqrt(dx * dx + dy * dy + dz * dz))
        t = wall_y / dy
        c = wall(dx * t, cam_z + dz * t)
        # Where the desk meets the wall: a soft contact shade.
        zz = cam_z + dz * t
        if zz < 30.0:
            k = 0.75 + 0.25 * zz / 30.0
            c = (c[0] * k, c[1] * k, c[2] * k)
        return c

    rows = []
    offsets = ((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75))   # 2 x 2 supersampling
    for j in range(h):
        row = bytearray()
        for i in range(w):
            acc = [0.0, 0.0, 0.0]
            for ox, oy in offsets:
                sy = (1.0 - 2.0 * (j + oy) / h) * tan_h
                sx = (2.0 * (i + ox) / w - 1.0) * tan_h * aspect
                c = shade(sx, sy)
                acc[0] += c[0]
                acc[1] += c[1]
                acc[2] += c[2]
            sy = (1.0 - 2.0 * (j + 0.5) / h) * tan_h
            sx = (2.0 * (i + 0.5) / w - 1.0) * tan_h * aspect
            # A gentle lens vignette.
            r2 = (sx / (tan_h * aspect)) ** 2 + (sy / tan_h) ** 2
            v = 0.25 * (1.0 - 0.12 * r2)
            row += bytes((srgb(acc[0] * v), srgb(acc[1] * v), srgb(acc[2] * v)))
        rows.append(row)
    write_png(out, w, h, rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

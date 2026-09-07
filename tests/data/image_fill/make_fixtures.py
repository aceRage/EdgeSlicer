#!/usr/bin/env python3
"""Regenerate the Image Fill test fixtures.

Every fixture is a minimal, hand-written 8-bit RGB PNG - no encoder library, so the bytes are
stable across machines and the SHA-256 the tests pin never moves. Run from the repo root:

    python tests/data/image_fill/make_fixtures.py

and the tests' pinned hashes stay valid because the bytes are byte-for-byte reproducible.
"""
import binascii
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def png(width, height, rows):
    """rows: list of `height` lists of `width` (r, g, b) tuples, 0..255."""
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type 0 (None), so the payload is literal pixel bytes
        for (r, g, b) in row:
            raw += bytes((r, g, b))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", binascii.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit, colour type 2 = RGB
    # A fixed compression level and strategy, so two runs produce identical bytes.
    comp = zlib.compressobj(level=9, method=zlib.DEFLATED, wbits=15, memLevel=8,
                            strategy=zlib.Z_DEFAULT_STRATEGY)
    idat = comp.compress(bytes(raw)) + comp.flush()
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) +
            chunk(b"IEND", b""))


def write(name, data):
    path = os.path.join(HERE, name)
    with open(path, "wb") as f:
        f.write(data)
    import hashlib
    print("%-24s %6d bytes  sha256=%s" % (name, len(data), hashlib.sha256(data).hexdigest()))


R = (255, 0, 0)
G = (0, 255, 0)
B = (0, 0, 255)
W = (255, 255, 255)
K = (0, 0, 0)

# quad_rgbw.png - four quadrants, so a projection test can say "this corner of the part must come
# out this colour". Row 0 is the TOP row of the image, which is v = 1 in the projection.
#   top-left  = red      top-right    = green
#   bot-left  = blue     bot-right    = white
write("quad_rgbw.png", png(2, 2, [[R, G], [B, W]]))

# stripes3.png - three horizontal bands, for the "tool changes appear where the image changes
# colour" slice check. 6 rows so each band is exactly two rows.
write("stripes3.png", png(3, 6, [[R] * 3, [R] * 3, [G] * 3, [G] * 3, [B] * 3, [B] * 3]))

# bands3.png - three VERTICAL bands, so the colour varies with u and not with v. The shape of the
# picture the projection bug was found with: a flat projection along Z must put these three bands
# on the top face and leave the four sides alone.
write("bands3.png", png(3, 1, [[R, G, B]]))

# wrap4.png - four bands around a wrap, offset by half a band so that each of a cube's four side
# faces falls in the MIDDLE of one band rather than across a boundary. Wrapped about Z the seam is
# on -X, u = 0.5 faces +X, and the eight columns give: -X white, -Y red, +X green, +Y blue.
write("wrap4.png", png(8, 1, [[W, R, R, G, G, B, B, W]]))

# ramp_kw.png - a 16-step black-to-white ramp along v, for monotonicity.
write("ramp_kw.png", png(1, 16, [[(v * 17, v * 17, v * 17)] for v in range(15, -1, -1)]))

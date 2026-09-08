"""Generate the ZAA dome test model: a 30 mm diameter hemisphere on a short plinth.

The wedge is a single constant slope (14.04 deg); the dome sweeps every slope from
0 deg at the pole to 90 deg at the equator, which is what exercises the perimeter
slope rule, the threshold crossing and the facet-to-facet raycast noise.

usage: make_dome.py <out.stl> [radius_mm] [plinth_mm] [nu] [nv] [y_scale]
"""
import math
import struct
import sys

out = sys.argv[1]
R = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0   # 30 mm diameter
PLINTH = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0

SY = float(sys.argv[6]) if len(sys.argv) > 6 else 1.0
NU = int(sys.argv[4]) if len(sys.argv) > 4 else 128   # around
NV = int(sys.argv[5]) if len(sys.argv) > 5 else 64    # pole to equator

tris = []


def sph(iu, iv):
    """Point on the hemisphere; iv = 0 is the equator, iv = NV is the pole."""
    phi = 2.0 * math.pi * iu / NU
    theta = 0.5 * math.pi * iv / NV
    r = R * math.cos(theta)
    return (r * math.cos(phi), SY * r * math.sin(phi), PLINTH + R * math.sin(theta))


def ring(iu, z):
    phi = 2.0 * math.pi * iu / NU
    return (R * math.cos(phi), SY * R * math.sin(phi), z)


# dome surface
for iv in range(NV):
    for iu in range(NU):
        a = sph(iu, iv)
        b = sph(iu + 1, iv)
        c = sph(iu + 1, iv + 1)
        d = sph(iu, iv + 1)
        if iv == NV - 1:
            tris.append((a, b, c))
        else:
            tris.append((a, b, c))
            tris.append((a, c, d))

# cylindrical plinth wall
for iu in range(NU):
    a = ring(iu, 0.0)
    b = ring(iu + 1, 0.0)
    c = ring(iu + 1, PLINTH)
    d = ring(iu, PLINTH)
    tris.append((a, b, c))
    tris.append((a, c, d))

# bottom disc
cen = (0.0, 0.0, 0.0)
for iu in range(NU):
    a = ring(iu, 0.0)
    b = ring(iu + 1, 0.0)
    tris.append((cen, b, a))


def normal(a, b, c):
    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    m = math.sqrt(sum(x * x for x in n)) or 1.0
    return (n[0] / m, n[1] / m, n[2] / m)


with open(out, "wb") as f:
    f.write(b"ZAA dome R=%d plinth=%d" % (int(R), int(PLINTH)))
    f.write(b"\0" * (80 - len(b"ZAA dome R=%d plinth=%d" % (int(R), int(PLINTH)))))
    f.write(struct.pack("<I", len(tris)))
    for a, b, c in tris:
        n = normal(a, b, c)
        f.write(struct.pack("<12fH", *n, *a, *b, *c, 0))

print("wrote", out, len(tris), "triangles; R =", R, "plinth =", PLINTH)

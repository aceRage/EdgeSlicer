import struct, math, sys

# 40 x 40 mm wedge, top rising from z = 2 mm at x = 0 to z = 12 mm at x = 40.
# atan(10/40) = 14.036 degrees (the brief calls it "15 degree").
L = 40.0
W = 40.0
Z0 = 2.0
Z1 = 12.0

b00 = (0.0, 0.0, 0.0)
b10 = (L,   0.0, 0.0)
b11 = (L,   W,   0.0)
b01 = (0.0, W,   0.0)
t00 = (0.0, 0.0, Z0)
t10 = (L,   0.0, Z1)
t11 = (L,   W,   Z1)
t01 = (0.0, W,   Z0)

tris = [
    # bottom (normal -Z)
    (b00, b11, b10), (b00, b01, b11),
    # ramp top
    (t00, t10, t11), (t00, t11, t01),
    # y = 0
    (b00, b10, t10), (b00, t10, t00),
    # y = W
    (b01, t01, t11), (b01, t11, b11),
    # x = 0
    (b00, t00, t01), (b00, t01, b01),
    # x = L
    (b10, b11, t11), (b10, t11, t10),
]

def normal(a, b, c):
    u = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
    v = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
    n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
    m = math.sqrt(sum(x*x for x in n)) or 1.0
    return (n[0]/m, n[1]/m, n[2]/m)

out = sys.argv[1]
with open(out, "wb") as f:
    f.write(b"ZAA wedge 40x40 2mm->12mm".ljust(80, b"\0"))
    f.write(struct.pack("<I", len(tris)))
    for a, b, c in tris:
        n = normal(a, b, c)
        f.write(struct.pack("<12fH", *n, *a, *b, *c, 0))
print("wrote", out, len(tris), "triangles; slope =",
      round(math.degrees(math.atan((Z1-Z0)/L)), 3), "deg")

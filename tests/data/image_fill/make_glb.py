#!/usr/bin/env python3
"""Two textured .glb files for the Image Fill Phase 2 before/after.

Both are deliberately LOW-POLY with a DETAILED texture, because that is the case the GLB import
status document's gap 4 names ("texture sampling is one colour per triangle; a detailed texture on
a low-poly mesh is quantised hard"). Written by hand from struct/zlib so nothing about them comes
from the reader they are used to measure.

  agent_plaque.glb    a 40 x 40 x 3 mm slab, 12 triangles, with a 64 x 64 six-colour texture
  agent_medallion.glb a 16-sided prism, 92 triangles, with a 64 x 64 four-band texture
"""
import binascii
import json
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


# ------------------------------------------------------------------ PNG
def png_rgb(width, height, pixel):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            r, g, b = pixel(x, y)
            raw += bytes((r & 255, g & 255, b & 255))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", binascii.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


# ------------------------------------------------------------------ GLB
def build_glb(positions, normals, uvs, indices, png_bytes, name):
    """One mesh, one primitive, one material with a baseColorTexture."""
    pos = b"".join(struct.pack("<fff", *p) for p in positions)
    nrm = b"".join(struct.pack("<fff", *n) for n in normals)
    uv = b"".join(struct.pack("<ff", *t) for t in uvs)
    idx = b"".join(struct.pack("<I", i) for i in indices)

    def pad4(b):
        return b + b"\x00" * ((4 - len(b) % 4) % 4)

    parts = [pad4(pos), pad4(nrm), pad4(uv), pad4(idx), pad4(png_bytes)]
    offsets, o = [], 0
    for p in parts:
        offsets.append(o)
        o += len(p)
    blob = b"".join(parts)

    mins = [min(p[i] for p in positions) for i in range(3)]
    maxs = [max(p[i] for p in positions) for i in range(3)]

    gltf = {
        "asset": {"version": "2.0", "generator": "image-fill phase 2 measurement fixture"},
        "scene": 0,
        "scenes": [{"name": name + " scene", "nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3,
             "material": 0, "mode": 4}]}],
        "materials": [{"name": "tex", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0}}],
        "textures": [{"source": 0}],
        "images": [{"bufferView": 4, "mimeType": "image/png"}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
             "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(pos), "target": 34962},
            {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(nrm), "target": 34962},
            {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(uv), "target": 34962},
            {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(idx), "target": 34963},
            {"buffer": 0, "byteOffset": offsets[4], "byteLength": len(png_bytes)},
        ],
        "buffers": [{"byteLength": len(blob)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    out = struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(js), 0x4E4F534A) + js
    out += struct.pack("<II", len(blob), 0x004E4942) + blob
    return out


# ------------------------------------------------------------------ 1. the plaque
# A 40 x 40 x 3 slab. glTF is Y-up, so the big faces are the +Y and -Y ones and the reader's
# (x, y, z) -> (x, -z, y) turns them into the top and bottom of the printed part.
def plaque():
    sx, sy, sz = 20.0, 1.5, 20.0
    # 8 corners, 6 faces, 2 triangles each, with UVs only meaningful on the +Y face (the rest
    # sample the texture's edges, which is fine and is what a real exporter does for a slab).
    P = [(-sx, -sy, -sz), (sx, -sy, -sz), (sx, -sy, sz), (-sx, -sy, sz),
         (-sx, sy, -sz), (sx, sy, -sz), (sx, sy, sz), (-sx, sy, sz)]
    faces = [
        ([4, 5, 6, 7], (0, 1, 0)),    # +Y, the face the picture is on
        ([1, 0, 3, 2], (0, -1, 0)),   # -Y
        ([3, 2, 6, 7], (0, 0, 1)),
        ([1, 0, 4, 5], (0, 0, -1)),
        ([2, 1, 5, 6], (1, 0, 0)),
        ([0, 3, 7, 4], (-1, 0, 0)),
    ]
    positions, normals, uvs, indices = [], [], [], []
    for quad, n in faces:
        base = len(positions)
        for k, vi in enumerate(quad):
            p = P[vi]
            positions.append(p)
            normals.append(n)
            # map x,z of the slab onto the full 0..1 square
            uvs.append(((p[0] + sx) / (2 * sx), (p[2] + sz) / (2 * sz)))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


# Six colours in a 3 x 2 arrangement, plus a fine 8-pixel checker on top of them, so a
# per-triangle sample and a per-sub-facet sample cannot possibly agree.
SIX = [(220, 30, 30), (30, 190, 60), (40, 70, 220),
       (240, 210, 40), (20, 20, 20), (245, 245, 245)]


def plaque_texel(x, y):
    cell = (y * 2 // 64) * 3 + (x * 3 // 64)
    r, g, b = SIX[cell % 6]
    if ((x // 8) + (y // 8)) % 2 == 0:
        return r, g, b
    # the alternate square is the same hue, darkened - still one of the six after quantisation on
    # a coarse palette, but a different sample on a fine one
    return r * 55 // 100, g * 55 // 100, b * 55 // 100


# ------------------------------------------------------------------ 2. the medallion
def medallion(sides=16, radius=15.0, half_h=2.0):
    import math
    positions, normals, uvs, indices = [], [], [], []
    # the wall
    for i in range(sides):
        a0 = 2 * math.pi * i / sides
        a1 = 2 * math.pi * (i + 1) / sides
        for (a, u) in ((a0, i / sides), (a1, (i + 1) / sides)):
            for h, v in ((-half_h, 1.0), (half_h, 0.0)):
                positions.append((radius * math.cos(a), h, radius * math.sin(a)))
                normals.append((math.cos(a), 0.0, math.sin(a)))
                uvs.append((u, v))
        b = len(positions) - 4
        indices += [b, b + 2, b + 3, b, b + 3, b + 1]
    # two caps, as fans around a centre vertex
    for h, n, flip in ((half_h, (0, 1, 0), False), (-half_h, (0, -1, 0), True)):
        c = len(positions)
        positions.append((0.0, h, 0.0))
        normals.append(n)
        uvs.append((0.5, 0.5))
        for i in range(sides + 1):
            a = 2 * math.pi * i / sides
            positions.append((radius * math.cos(a), h, radius * math.sin(a)))
            normals.append(n)
            uvs.append((0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a)))
        for i in range(sides):
            if flip:
                indices += [c, c + 1 + i + 1, c + 1 + i]
            else:
                indices += [c, c + 1 + i, c + 1 + i + 1]
    return positions, normals, uvs, indices


def medallion_texel(x, y):
    # four horizontal bands with a fine vertical ripple, so a wall quad (two triangles spanning a
    # 1/16 slice of u) covers several distinct colours.
    band = [(230, 40, 40), (250, 200, 30), (30, 170, 80), (40, 60, 210)][y * 4 // 64]
    k = 1.0 if (x // 4) % 2 == 0 else 0.5
    return int(band[0] * k), int(band[1] * k), int(band[2] * k)


def main():
    tex1 = png_rgb(64, 64, plaque_texel)
    p, n, uv, idx = plaque()
    glb1 = build_glb(p, n, uv, idx, tex1, "agent plaque")
    open(os.path.join(HERE, "agent_plaque.glb"), "wb").write(glb1)
    print("agent_plaque.glb    %6d bytes, %d triangles, %d x %d texture" %
          (len(glb1), len(idx) // 3, 64, 64))

    tex2 = png_rgb(64, 64, medallion_texel)
    p, n, uv, idx = medallion()
    glb2 = build_glb(p, n, uv, idx, tex2, "agent medallion")
    open(os.path.join(HERE, "agent_medallion.glb"), "wb").write(glb2)
    print("agent_medallion.glb %6d bytes, %d triangles, %d x %d texture" %
          (len(glb2), len(idx) // 3, 64, 64))


if __name__ == "__main__":
    main()

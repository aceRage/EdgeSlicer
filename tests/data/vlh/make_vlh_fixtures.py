#!/usr/bin/env python3
"""Generate the variable layer height (VLH) fixture for G-code gates.

vlh_t.3mf is a plain core-spec 3MF holding ONE object: a 10 x 10 x 10 mm pillar under a
30 x 30 x 2 mm plate (top at z = 12 mm), so the plate's underside is a large flat overhang. It
carries a layer height profile in Metadata/layer_heights_profile.txt, the file Orca/Bambu
projects keep it in ("object_id=<1-based object index>|z;h;z;h;..."):

    z (mm)  0     3     6     9     12
    h (mm)  0.20  0.20  0.12  0.15  0.24

The heights are interpolated linearly between those points. What the fixture exercises:

  - Snapmaker #757: the profile's first segment is 0.20 mm. A printer whose first layer is not
    0.20 mm (the U1's 0.20mm Standard uses 0.25 mm) used to throw the whole profile away and
    slice at a fixed layer height; with a 0.20 mm first layer (X1C) the profile was kept before too.
  - Snapmaker #696: around the plate's underside (z = 10 mm) the layers are about 0.18 mm, so a
    0.2 mm top Z distance syncs down to one 0.18 mm layer and the object layer the top contact layer
    supports falls inside the trimming window (normal supports, independent support layer height
    off, as on the X1C and the Voron).

The profile's last z must equal the object height, or the slicer discards it, so the geometry and
the profile are defined together here.

    python tests/data/vlh/make_vlh_fixtures.py

Regenerating is deterministic (fixed timestamps), so the committed file can be checked against a
fresh run.
"""
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
 <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
 <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>
"""

RELS = """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>
"""

PROFILE = [(0., 0.20), (3., 0.20), (6., 0.12), (9., 0.15), (12., 0.24)]


def box(x0, y0, z0, dx, dy, dz, base=0):
    """Axis-aligned box as (vertices, triangles) with outward-facing winding."""
    v = [(x0, y0, z0), (x0 + dx, y0, z0), (x0 + dx, y0 + dy, z0), (x0, y0 + dy, z0),
         (x0, y0, z0 + dz), (x0 + dx, y0, z0 + dz), (x0 + dx, y0 + dy, z0 + dz), (x0, y0 + dy, z0 + dz)]
    t = [(0, 2, 1), (0, 3, 2),          # bottom
         (4, 5, 6), (4, 6, 7),          # top
         (0, 1, 5), (0, 5, 4),          # -Y
         (1, 2, 6), (1, 6, 5),          # +X
         (2, 3, 7), (2, 7, 6),          # +Y
         (3, 0, 4), (3, 4, 7)]          # -X
    return v, [(a + base, b + base, c + base) for a, b, c in t]


def mesh_xml(vertices, triangles, indent="   "):
    out = [indent + "<mesh>", indent + " <vertices>"]
    for x, y, z in vertices:
        out.append('%s  <vertex x="%g" y="%g" z="%g"/>' % (indent, x, y, z))
    out.append(indent + " </vertices>")
    out.append(indent + " <triangles>")
    for a, b, c in triangles:
        out.append('%s  <triangle v1="%d" v2="%d" v3="%d"/>' % (indent, a, b, c))
    out.append(indent + " </triangles>")
    out.append(indent + "</mesh>")
    return "\n".join(out)


def main():
    pillar_v, pillar_t = box(-5, -5, 0, 10, 10, 10)
    plate_v, plate_t = box(-15, -15, 10, 30, 30, 2, base=len(pillar_v))
    height = 12.
    assert PROFILE[-1][0] == height
    model = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<model unit="millimeter" xml:lang="en-US" '
        'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">\n'
        ' <resources>\n'
        '  <object id="1" name="vlh_t" type="model">\n%s\n  </object>\n'
        ' </resources>\n'
        ' <build>\n  <item objectid="1"/>\n </build>\n'
        '</model>\n' % mesh_xml(pillar_v + plate_v, pillar_t + plate_t))
    profile = "object_id=1|" + ";".join("%f;%f" % zh for zh in PROFILE) + "\n"

    files = [("[Content_Types].xml", CONTENT_TYPES),
             ("_rels/.rels", RELS),
             ("3D/3dmodel.model", model),
             ("Metadata/layer_heights_profile.txt", profile)]
    path = os.path.join(HERE, "vlh_t.3mf")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files:
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 13, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o600 << 16
            z.writestr(info, data)
    print("wrote", path, os.path.getsize(path), "bytes")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Multi-colour filaments (P3): the P0 calibration shapes as Showcase fixture projects.

Builds one project 3MF per shape and view from a template project (an EdgeSlicer project 3MF with
Metadata/project_settings.config, e.g. the U1 project tests/data/support_corpus/onepart_group.3mf). The template's
objects are replaced by the shape, slot 1 becomes a dual-colour red/yellow silk, and a Showcase scene
(toolpaths, Studio white, silk) is added. tests/design_multicolour_filament_view.md 7.2 and 8.2;
the photos they are compared with are in tests/p0_multicolour_cc1 (the owner's CC1 prints).

    python -I make_multicolour_fixtures.py <template.3mf> <out dir> [--angle DEG] [--colors "#RRGGBB|#RRGGBB"]
                                           [--source toolpaths|model]

Writes <out dir>/<shape>_<view>.3mf and lists them on stdout, one per line.

--angle is the slot's rotation (filament_colour_angle, D3: where the middle of colour 1 faces,
anticlockwise from the printer's front seen from above). The default, -45, puts the boundary at the
front-right corner, as on the P0 cube (front face red, right face yellow); set it from the pen mark.

--source is the scene's source: "toolpaths" (P3, the default) or "model" (P4, Showcase mesh mode: the
coextruded look without a slice).
"""
import argparse
import json
import math
import os
import sys
import zipfile

SCENES_ENTRY = "Metadata/showcase/scenes.json"
SETTINGS_ENTRY = "Metadata/project_settings.config"
MODEL_ENTRY = "3D/3dmodel.model"
OBJECT_ENTRY = "3D/Objects/object_1.model"
RELS_ENTRY = "3D/_rels/3dmodel.model.rels"
MODEL_SETTINGS_ENTRY = "Metadata/model_settings.config"

NS = ('xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
      'xmlns:BambuStudio="http://schemas.bambulab.com/package/2021" '
      'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p"')


# ---- meshes: (vertices, triangles), z from 0, centred on x = y = 0 ----

def box(sx, sy, sz):
    hx, hy = sx / 2.0, sy / 2.0
    v = [(-hx, -hy, 0), (hx, -hy, 0), (hx, hy, 0), (-hx, hy, 0),
         (-hx, -hy, sz), (hx, -hy, sz), (hx, hy, sz), (-hx, hy, sz)]
    t = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    return v, t


def cylinder(r, h, segments=180):
    v = [(0.0, 0.0, 0.0), (0.0, 0.0, h)]
    for i in range(segments):
        a = 2.0 * math.pi * i / segments
        v.append((r * math.cos(a), r * math.sin(a), 0.0))
        v.append((r * math.cos(a), r * math.sin(a), h))
    t = []
    for i in range(segments):
        j = (i + 1) % segments
        b0, t0, b1, t1 = 2 + 2 * i, 3 + 2 * i, 2 + 2 * j, 3 + 2 * j
        t += [(0, b1, b0), (1, t0, t1), (b0, b1, t1), (b0, t1, t0)]
    return v, t


# ---- the shapes of the P0 print (tests/p0_multicolour_cc1/NOTES.md) and their settings ----

SHAPES = {
    # 30 mm solid cylinder: walls sweep round, zig-zag top stripes.
    "cylinder30": (lambda: cylinder(15.0, 30.0), {"top_surface_pattern": "monotonic", "top_layer_direction": "45"}),
    # 20 mm cube: one colour per face, diagonal top stripes.
    "cube20": (lambda: box(20.0, 20.0, 20.0), {"top_surface_pattern": "monotonic", "top_layer_direction": "45"}),
    # 40 x 40 x 2 plaques: every top line the same way at 0 degrees; alternating at 45 degrees.
    "plaque_monotonic_0": (lambda: box(40.0, 40.0, 2.0), {"top_surface_pattern": "monotonicline", "top_layer_direction": "0"}),
    "plaque_rectilinear_45": (lambda: box(40.0, 40.0, 2.0), {"top_surface_pattern": "rectilinear", "top_layer_direction": "45"}),
    # vase-mode cylinder, 30 mm across, 40 mm tall.
    "vase30": (lambda: cylinder(15.0, 40.0), {"spiral_mode": "1", "wall_loops": "1", "top_shell_layers": "0",
                                               "top_shell_thickness": "0", "sparse_infill_density": "0%",
                                               "enable_support": "0"}),
}

# Views: three-quarter (Showcase's own default framing), straight down, and from the back left.
VIEWS = {
    "cylinder30": ["threequarter", "top", "backleft"],
    "cube20": ["threequarter", "top", "backleft"],
    "plaque_monotonic_0": ["top", "threequarter"],
    "plaque_rectilinear_45": ["top", "threequarter"],
    "vase30": ["threequarter", "top", "backleft"],
}
FRAME_HEIGHT = 1080


def camera(view, centre, extent, height):
    cx, cy = centre
    if view == "threequarter":
        return None   # Showcase frames the model from the front right, a little above
    if view == "top":
        # Straight down, the printer's front at the bottom of the image (a hair off vertical, with Z
        # up, so the camera keeps +Y up: a view exactly along its up vector is ill-defined).
        return {"projection": "ortho", "eye": [cx, cy - 0.5, height + 150.0], "target": [cx, cy, height],
                "up": [0.0, 0.0, 1.0], "zoom": 0.7 * FRAME_HEIGHT / extent}
    if view == "backleft":
        d = 4.0 * extent + 60.0
        e = [cx - 0.55 * d, cy + 0.75 * d, height * 0.5 + 0.45 * d]
        return {"projection": "ortho", "eye": e, "target": [cx, cy, height * 0.5], "up": [0.0, 0.0, 1.0],
                "zoom": 0.55 * FRAME_HEIGHT / (extent * 1.4)}
    raise ValueError(view)


def object_model(mesh):
    v, t = mesh
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', '<model unit="millimeter" xml:lang="en-US" %s>' % NS,
             ' <metadata name="BambuStudio:3mfVersion">1</metadata>', ' <resources>',
             '  <object id="1" p:UUID="00010000-81cb-4c03-9d28-80fed5dfa1dc" type="model">', '   <mesh>', '    <vertices>']
    lines += ['     <vertex x="%.6f" y="%.6f" z="%.6f"/>' % p for p in v]
    lines += ['    </vertices>', '    <triangles>']
    lines += ['     <triangle v1="%d" v2="%d" v3="%d"/>' % tri for tri in t]
    lines += ['    </triangles>', '   </mesh>', '  </object>', ' </resources>', ' <build/>', '</model>', '']
    return "\n".join(lines)


def root_model(name, cx, cy):
    tr = "1 0 0 0 1 0 0 0 1 %.6f %.6f 0" % (cx, cy)
    return "\n".join([
        '<?xml version="1.0" encoding="UTF-8"?>', '<model unit="millimeter" xml:lang="en-US" %s>' % NS,
        ' <metadata name="Application">EdgeSlicer</metadata>', ' <metadata name="BambuStudio:3mfVersion">1</metadata>',
        ' <metadata name="Title">%s</metadata>' % name, ' <resources>',
        '  <object id="2" p:UUID="00000001-61cb-4c03-9d28-80fed5dfa1dc" type="model">', '   <components>',
        '    <component p:path="/%s" objectid="1" p:UUID="00010000-b206-40ff-9872-83e8017abed1" transform="1 0 0 0 1 0 0 0 1 0 0 0"/>' % OBJECT_ENTRY,
        '   </components>', '  </object>', ' </resources>', ' <build p:UUID="2c7c17d8-22b5-4d84-8835-1976022ea369">',
        '  <item objectid="2" p:UUID="00000002-b1ec-4553-aec9-835e5b724bb4" transform="%s" printable="1"/>' % tr,
        ' </build>', '</model>', ''])


def rels():
    return "\n".join([
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">',
        ' <Relationship Target="/%s" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>' % OBJECT_ENTRY,
        '</Relationships>', ''])


def model_settings(name, cx, cy):
    return "\n".join([
        '<?xml version="1.0" encoding="UTF-8"?>', '<config>', '  <object id="2">',
        '    <metadata key="name" value="%s"/>' % name, '    <metadata key="extruder" value="1"/>',
        '    <part id="1" subtype="normal_part">', '      <metadata key="name" value="%s"/>' % name,
        '      <metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>', '    </part>', '  </object>',
        '  <plate>', '    <metadata key="plater_id" value="1"/>', '    <metadata key="plater_name" value=""/>',
        '    <metadata key="locked" value="false"/>', '    <model_instance>', '      <metadata key="object_id" value="2"/>',
        '      <metadata key="instance_id" value="0"/>', '      <metadata key="identify_id" value="302"/>',
        '    </model_instance>', '  </plate>', '  <assemble>',
        '   <assemble_item object_id="2" instance_id="0" transform="1 0 0 0 1 0 0 0 1 %.6f %.6f 0" offset="0 0 0" />' % (cx, cy),
        '  </assemble>', '</config>', ''])


def bed_centre(settings):
    pts = []
    for p in settings.get("printable_area", ["0x0", "200x0", "200x200", "0x200"]):
        x, y = p.split("x")
        pts.append((float(x), float(y)))
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0


def set_slot0(settings, key, value):
    cur = settings.get(key)
    if isinstance(cur, list) and cur:
        cur[0] = value
    else:
        n = len(settings.get("filament_colour", [""]))
        settings[key] = [value] + ([cur[1] if isinstance(cur, list) and len(cur) > 1 else value] * (n - 1) if n > 1 else [])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("template")
    ap.add_argument("out")
    ap.add_argument("--angle", type=float, default=-45.0)
    ap.add_argument("--colors", default="#D7261E|#F5BE1B")
    ap.add_argument("--twist", type=float, default=0.0)
    ap.add_argument("--source", choices=["toolpaths", "model"], default="toolpaths")
    a = ap.parse_args(argv[1:])

    with zipfile.ZipFile(a.template) as z:
        template = {i.filename: z.read(i.filename) for i in z.infolist()}
    base_settings = json.loads(template[SETTINGS_ENTRY].decode("utf-8"))
    os.makedirs(a.out, exist_ok=True)
    cx, cy = bed_centre(base_settings)
    first = a.colors.split("|")[0]

    for shape, (make_mesh, overrides) in SHAPES.items():
        mesh = make_mesh()
        xs = [p[0] for p in mesh[0]]
        zs = [p[2] for p in mesh[0]]
        extent = max(xs) - min(xs)
        height = max(zs)
        settings = json.loads(json.dumps(base_settings))
        for k, v in overrides.items():
            settings[k] = v
        # The loader rebuilds the process from its system preset plus the keys listed as changed:
        # list ours, or they fall back to the preset's values.
        diffs = settings.get("different_settings_to_system")
        if isinstance(diffs, list) and diffs:
            keys = [k for k in diffs[0].split(";") if k]
            keys += [k for k in overrides if k not in keys]
            diffs[0] = ";".join(sorted(keys))
        set_slot0(settings, "filament_colour", first)
        set_slot0(settings, "filament_multi_colors", a.colors)
        set_slot0(settings, "filament_colour_mode", "0")
        set_slot0(settings, "filament_colour_split", "")
        set_slot0(settings, "filament_colour_angle", ("%g" % a.angle))
        set_slot0(settings, "filament_colour_twist", ("%g" % a.twist))
        for view in VIEWS[shape]:
            name = "%s_%s" % (shape, view)
            scene = {"id": "plate1", "name": ("P3" if a.source == "toolpaths" else "P4") + " multi-colour: " + name, "plate_index": 0, "source": a.source,
                     "preset_base": "studio_white", "view_frame_height": FRAME_HEIGHT,
                     "background": {"type": "radial_gradient", "color1": "#FFFFFF", "color2": "#D9DCE1"},
                     "floor": {"type": "shadow_catcher", "color": "#F2F2F2", "shadow_strength": 0.45},
                     "materials": {"filaments": [{"slot": 1, "finish": "silk"}]},
                     "export": {"width": 1920, "height": 1080, "quality": "high", "format": "png"}}
            cam = camera(view, (cx, cy), extent, height)
            if cam is not None:
                scene["camera"] = cam
            scenes = {"schema": 2, "active": "plate1", "scenes": [scene]}
            out = os.path.join(a.out, name + ".3mf")
            with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as dst:
                for fn, data in template.items():
                    if fn.startswith("3D/") or fn.startswith("Metadata/") and fn.endswith(".png"):
                        continue
                    if fn in (SETTINGS_ENTRY, MODEL_SETTINGS_ENTRY, SCENES_ENTRY, "Metadata/slice_info.config"):
                        continue
                    dst.writestr(fn, data)
                dst.writestr(MODEL_ENTRY, root_model(shape, cx, cy))
                dst.writestr(RELS_ENTRY, rels())
                dst.writestr(OBJECT_ENTRY, object_model(mesh))
                dst.writestr(MODEL_SETTINGS_ENTRY, model_settings(shape, cx, cy))
                dst.writestr(SETTINGS_ENTRY, json.dumps(settings, indent=4))
                dst.writestr(SCENES_ENTRY, json.dumps(scenes, indent=2))
            print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

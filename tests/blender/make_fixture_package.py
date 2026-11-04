#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A small EdgeSlicer Blender package written without EdgeSlicer, for testing the add-on on its own.

It follows the format EdgeSlicer writes (src/libslic3r/Showcase/ShowcaseBlender.cpp, format
"edgeslicer-blender-package" version 1): a 20 mm silk cube, a transparent blue ball and a sparkle
bar on a shadow catcher, three suns and the camera 70 mm of frame high. With --video it also writes a
12-frame camera path with a build-up. The real exporter's packages come from the Catch2 case
"[ShowcaseBlender][fixture]" (EDGESLICER_BLENDER_FIXTURE_OUT); run_blender_tests.py uses both.

    python make_fixture_package.py <out folder> [--video] [--hdri <file.hdr>]
"""

import argparse
import json
import math
import os
import shutil
import struct

MM = 0.001


def box(x0, y0, z0, x1, y1, z1):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    q = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    f = []
    for a, b, c, d in q:
        f += [(a, b, c), (a, c, d)]
    return v, f


def sphere(cx, cy, cz, r, seg=32):
    rings = seg // 2
    v = []
    for i in range(rings + 1):
        for j in range(seg):
            th, ph = math.pi * i / rings, 2 * math.pi * j / seg
            v.append((cx + r * math.sin(th) * math.cos(ph), cy + r * math.sin(th) * math.sin(ph), cz + r * math.cos(th)))
    f = []
    for i in range(rings):
        for j in range(seg):
            a, c = i * seg + j, i * seg + (j + 1) % seg
            d, e = (i + 1) * seg + j, (i + 1) * seg + (j + 1) % seg
            if i > 0:
                f.append((a, d, c))
            if i + 1 < rings:
                f.append((c, d, e))
    return v, f


def to_gltf(p):
    x, y, z = p
    return (x * MM, z * MM, -y * MM)


def write_glb(path, parts):
    """parts: [(name, vertices mm, faces, material index, extras)], materials: list of dicts."""
    bin_ = bytearray()
    views, accessors, meshes, nodes = [], [], [], []

    def add(data, target):
        while len(bin_) % 4:
            bin_.append(0)
        views.append({"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data), "target": target})
        bin_.extend(data)
        return len(views) - 1

    for name, verts, faces, mat, extras in parts["parts"]:
        pos = [to_gltf(p) for p in verts]
        pdata = b"".join(struct.pack("<3f", *p) for p in pos)
        pv = add(pdata, 34962)
        lo = [min(p[i] for p in pos) for i in range(3)]
        hi = [max(p[i] for p in pos) for i in range(3)]
        accessors.append({"bufferView": pv, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": lo, "max": hi})
        pa = len(accessors) - 1
        idata = b"".join(struct.pack("<3I", *f) for f in faces)
        iv = add(idata, 34963)
        accessors.append({"bufferView": iv, "componentType": 5125, "count": 3 * len(faces), "type": "SCALAR"})
        ia = len(accessors) - 1
        meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": pa}, "indices": ia, "material": mat, "mode": 4}]})
        nodes.append({"name": name, "mesh": len(meshes) - 1, "extras": extras})
    while len(bin_) % 4:
        bin_.append(0)
    doc = {
        "asset": {"version": "2.0", "generator": "EdgeSlicer test fixture"},
        "buffers": [{"byteLength": len(bin_)}],
        "bufferViews": views,
        "accessors": accessors,
        "materials": parts["materials"],
        "meshes": meshes,
        "nodes": nodes,
        "scenes": [{"name": "EdgeSlicer Showcase", "nodes": list(range(len(nodes)))}],
        "scene": 0,
    }
    js = json.dumps(doc, sort_keys=True, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    total = 12 + 8 + len(js) + 8 + len(bin_)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942))
        f.write(bin_)


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hexlin(h):
    return [round(srgb_to_linear(int(h[i:i + 2], 16) / 255.0), 6) for i in (1, 3, 5)]


def material(name, finish, color, **kw):
    spec = {
        "finish": finish, "base_color": hexlin(color), "roughness": 0.55, "metallic": 0.0, "ior": 1.5, "specular_tint": 0.0,
        "anisotropic": 0.0, "tangent": "radial_z", "coat_weight": 0.0, "coat_roughness": 0.05, "sheen_weight": 0.0,
        "sheen_roughness": 0.5, "transmission_weight": 0.0, "thickness_m": 0.0, "absorption_density": 0.0,
        "subsurface_weight": 0.0, "subsurface_scale_m": 0.0, "subsurface_radius": [1, 1, 1], "emission_color": [0, 0, 0],
        "emission_strength": 0.0, "sparkle": 0.0, "sparkle_cell_m": 0.0, "fiber_noise": 0.0, "speckle": "none",
        "speckle_amount": 0.0, "layer_bump": 0.0, "color": color,
    }
    spec.update(kw)
    gltf = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": spec["base_color"] + [1.0], "metallicFactor": spec["metallic"],
                                                   "roughnessFactor": spec["roughness"]},
            "extras": {"edgeslicer": {"finish": finish, "blender": spec}}}
    return spec, gltf


def look_at_fov(eye, target, view_h_mm):
    d = math.dist(eye, target)
    return 2 * math.degrees(math.atan(0.5 * view_h_mm / d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--video", action="store_true")
    ap.add_argument("--hdri")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    layers = {"valid": True, "uniform": True, "first_m": 0.0002, "height_m": 0.0002}
    silk, silk_g = material("EdgeSlicer F1 silk", "silk", "#C0C0C0", roughness=0.35, ior=1.65, specular_tint=0.6, anisotropic=0.75,
                            layer_bump=0.045)
    clear, clear_g = material("EdgeSlicer F2 transparent", "transparent", "#3070FF", roughness=0.25, ior=1.57, coat_weight=0.3,
                              transmission_weight=0.9, thickness_m=0.0016, absorption_density=120.0, subsurface_weight=0.1,
                              subsurface_scale_m=0.0025)
    spark, spark_g = material("EdgeSlicer F3 sparkle", "sparkle", "#202040", roughness=0.38, ior=1.58, coat_weight=0.35, sparkle=0.35,
                              sparkle_cell_m=0.00015)
    cube = box(0, 0, 0, 20, 20, 20)
    ball = sphere(40, 10, 10, 10)
    bar = box(-25, 0, 0, -15, 10, 30)
    parts = {
        "materials": [silk_g, clear_g, spark_g],
        "parts": [
            ("Cube", cube[0], cube[1], 0, {"edgeslicer": {"kind": "part", "filament": 1, "layers": layers}}),
            ("Ball", ball[0], ball[1], 1, {"edgeslicer": {"kind": "part", "filament": 2}}),
            ("Bar", bar[0], bar[1], 2, {"edgeslicer": {"kind": "part", "filament": 3}}),
        ],
    }
    write_glb(os.path.join(a.out, "scene.glb"), parts)

    eye, target = (90.0, -140.0, 80.0), (10.0, 10.0, 10.0)
    fov = look_at_fov(eye, target, 70.0)
    aspect = 480 / 270
    half_sensor = 21.633307652783937 / math.sqrt(1 + aspect * aspect)
    lens = half_sensor / math.tan(math.radians(fov) / 2)
    env_file = None
    if a.hdri:
        os.makedirs(os.path.join(a.out, "environment"), exist_ok=True)
        env_file = "environment/hdri_studio.hdr"
        shutil.copyfile(a.hdri, os.path.join(a.out, env_file))
    camera = {"type": "persp", "location": [e * MM for e in eye], "target": [t * MM for t in target], "up": [0, 0, 1],
              "fov_y_deg": fov, "ortho_height": 0.07, "sensor_fit": "VERTICAL", "sensor_height_mm": 2 * half_sensor, "lens_mm": lens,
              "clip_start": 0.0005, "clip_end": 2000,
              "dof": {"enabled": True, "f_number": 4.0, "focus_on_model": True, "focus_point": [0.0125, 0.01, 0.015], "focus_distance": 0.2}}

    def sun(name, az, el, strength):
        az, el = math.radians(az), math.radians(el)
        return {"name": name, "type": "SUN", "direction": [math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el)],
                "color": [1, 0.96, 0.9], "strength": strength, "angle_deg": 4.5}

    data = {
        "format": "edgeslicer-blender-package", "version": 1, "generator": "EdgeSlicer test fixture",
        "units": {"length": "m", "up": "+Z", "metres_per_mm": 0.001}, "glb": "scene.glb", "source": "model",
        "scene_name": "Fixture",
        "objects": [{"node": "Cube", "kind": "part", "filament": 1, "material": "EdgeSlicer F1 silk", "layers": layers},
                    {"node": "Ball", "kind": "part", "filament": 2, "material": "EdgeSlicer F2 transparent", "layers": {"valid": False}},
                    {"node": "Bar", "kind": "part", "filament": 3, "material": "EdgeSlicer F3 sparkle", "layers": {"valid": False}}],
        "materials": {"EdgeSlicer F1 silk": silk, "EdgeSlicer F2 transparent": clear, "EdgeSlicer F3 sparkle": spark},
        "output": {"width": 480, "height": 270, "fps": 30, "frames": 12 if a.video else 0, "transparent": False},
        "look": {"preset": "studio_white", "tone": "neutral", "exposure": 0.0, "quality": "high",
                 "materials": {"layer_bump": True, "imperfections": {"colour": 0.3}}},
        "camera": camera,
        "lights": [sun("EdgeSlicer Key", 225, 55, 3.14), sun("EdgeSlicer Fill", 335, 20, 1.1), sun("EdgeSlicer Rim", 30, 50, 1.4)],
        "shadows": True,
        "environment": {"id": "hdri_studio" if env_file else "studio", "file": env_file, "rotation_deg": 30.0,
                        "strength": 0.7 if env_file else 0.7, "radiance_scale": 1.0, "intensity": 0.7, "fallback_color": [0.7, 0.72, 0.75]},
        "background": {"type": "environment" if env_file else "radial_gradient", "color1": [1, 1, 1], "color2": [0.69, 0.71, 0.75],
                       "blur": 0, "image": None, "fit": "fill", "backdrop_pitch": 8.0, "ground": {"enabled": False}},
        "floor": {"type": "shadow_catcher", "chosen_type": "shadow_catcher", "z": 0, "center": [0.0125, 0.01, 0], "size": 4.0,
                  "color": [0.9, 0.9, 0.9], "shadow_strength": 0.45, "grid_spacing": 0.01, "image": None, "tile": 0.5, "rotation": 0,
                  "flip": False, "roughness": 0.6, "reflection": 0, "contact_shadow": 0.6},
        "model": {"min": [-0.025, 0, 0], "max": [0.05, 0.02, 0.03], "center": [0.0125, 0.01, 0.015]},
        "overlay": None,
        "camera_path": "camera_path.json" if a.video else None,
    }
    with open(os.path.join(a.out, "showcase.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)
    if a.video:
        frames = []
        for i in range(12):
            ang = 2 * math.pi * i / 12
            e = (12.5 + 160 * math.cos(ang - 1.0), 10 + 160 * math.sin(ang - 1.0), 80.0)
            frames.append({"location": [c * MM for c in e], "target": [t * MM for t in target], "up": [0, 0, 1], "fov_y_deg": fov,
                           "lens_mm": lens, "ortho_height": 0.07, "spin_deg": 0.0, "build_height": 0.03 * (i + 1) / 12})
        with open(os.path.join(a.out, "camera_path.json"), "w", encoding="utf-8") as f:
            json.dump({"format": "edgeslicer-blender-camera-path", "version": 1, "fps": 30, "turntable_center": [0.0125, 0.01, 0],
                       "frames": frames}, f, indent=1, sort_keys=True)
    with open(os.path.join(a.out, "README.txt"), "w", encoding="utf-8") as f:
        f.write("EdgeSlicer Blender package test fixture.\n")


if __name__ == "__main__":
    main()

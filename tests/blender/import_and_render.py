# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs inside Blender (design 5.7 checks):

    blender -b --factory-startup --python import_and_render.py -- --package <p> --out <folder>
            [--samples 16] [--width 320]

Registers the add-on from resources/blender for this session (as Open in Blender does), imports the
package, checks the scene (objects, finished materials, camera, lights, world, the video's keys and
build-up), renders one still at a low sample count and checks it is not black. Writes report.json
and still.png into --out; the exit code is the number of failed checks (0 = all passed).
"""

import argparse
import json
import os
import sys
import traceback

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
RESOURCES = os.path.normpath(os.path.join(HERE, "..", "..", "resources", "blender"))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--samples", type=int, default=16)
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--frame", type=int, default=0, help="render this frame of a video (0 = the middle one)")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)

    sys.dont_write_bytecode = True   # no __pycache__ in resources/blender
    sys.path.insert(0, RESOURCES)
    import edgeslicer_render

    edgeslicer_render.register()
    for name in ("Cube", "Camera", "Light"):
        obj = bpy.data.objects.get(name)
        if obj is not None:
            bpy.data.objects.remove(obj, do_unlink=True)

    report = {"blender": bpy.app.version_string, "package": a.package, "checks": {}, "errors": []}
    checks = report["checks"]

    summary = edgeslicer_render.importer.import_package(a.package, "PREVIEW", "CPU", a.samples)
    report["summary"] = summary
    sc = bpy.context.scene
    pkg = edgeslicer_render.package.load(a.package)

    col = bpy.data.collections.get("EdgeSlicer Showcase")
    meshes = [o for o in col.all_objects if o.type == "MESH" and o.get("edgeslicer_kind")]
    checks["objects_match_package"] = len(meshes) == len(pkg.get("objects", default=[]))
    finished = [o for o in meshes if o.data.materials and o.data.materials[0] and o.data.materials[0].get("edgeslicer_finish")]
    checks["every_part_has_a_finish_material"] = len(finished) == len(meshes) and len(meshes) > 0
    report["finishes"] = sorted({o.data.materials[0]["edgeslicer_finish"] for o in finished})
    checks["camera"] = sc.camera is not None and sc.camera.name == "EdgeSlicer Camera"
    checks["dof_follows_package"] = sc.camera.data.dof.use_dof == bool(pkg.get("camera", "dof", "enabled", default=False))
    checks["lights"] = len([o for o in col.all_objects if o.type == "LIGHT"]) == len(pkg.get("lights", default=[]))
    checks["world"] = sc.world is not None and sc.world.name == "EdgeSlicer World"
    checks["cycles"] = sc.render.engine == "CYCLES"
    report["view_transform"] = sc.view_settings.view_transform
    checks["tone"] = sc.view_settings.view_transform in ("Khronos PBR Neutral", "Filmic", "AgX")
    # Units: the parts are where EdgeSlicer put them, in metres.
    mn = [min((o.matrix_world @ v.co)[i] for o in meshes for v in o.data.vertices) for i in range(3)]
    mx = [max((o.matrix_world @ v.co)[i] for o in meshes for v in o.data.vertices) for i in range(3)]
    want_mn, want_mx = pkg.get("model", "min", default=None), pkg.get("model", "max", default=None)
    report["bounds"] = [mn, mx]
    if want_mn and want_mx and pkg.get("source") == "model":
        checks["units_metres"] = all(abs(mn[i] - want_mn[i]) < 1e-4 and abs(mx[i] - want_mx[i]) < 1e-4 for i in range(3))
    else:
        checks["units_metres"] = max(mx) - min(mn) < 1.0   # a print is less than a metre across
    # Toolpaths: the extrusion direction arrives in Blender's Z-up frame (flat walls run horizontally).
    silk = [o.data.attributes.get("_SILK_DIR") for o in meshes if o.data.attributes.get("_SILK_DIR") is not None]
    if silk:
        zs = [abs(d.vector[2]) for a in silk for d in a.data]
        checks["silk_dir_z_up"] = sum(zs) / max(1, len(zs)) < 0.2
    frames = pkg.frames
    if frames:
        cam_curves = edgeslicer_render.compat.action_fcurves(sc.camera)
        keys = max((len(fc.keyframe_points) for fc in cam_curves), default=0)
        report["camera_keys"] = keys
        checks["camera_keyframes"] = keys == len(frames)
        checks["frame_range"] = sc.frame_end - sc.frame_start + 1 == len(frames)
        if any(f.get("build_height") is not None for f in frames):
            checks["build_up_keyed"] = "edgeslicer_build_height" in sc.keys() and any(
                fc.data_path == '["edgeslicer_build_height"]' for fc in edgeslicer_render.compat.action_fcurves(sc))
        sc.frame_set(a.frame if a.frame > 0 else max(1, len(frames) // 2))

    # One still at a low sample count.
    sc.cycles.samples = a.samples
    sc.cycles.use_denoising = False
    out_png = os.path.join(a.out, "still.png")
    sc.render.resolution_percentage = max(1, int(round(100.0 * a.width / max(1, sc.render.resolution_x))))
    sc.render.filepath = out_png
    sc.render.image_settings.file_format = "PNG"
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(out_png)
    px = list(img.pixels)
    n = len(px) // 4
    mean = sum(0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2] for i in range(n)) / max(1, n)
    alpha = sum(px[i * 4 + 3] for i in range(n)) / max(1, n)
    report["render"] = {"file": out_png, "size": list(img.size), "mean_luma": mean, "mean_alpha": alpha}
    checks["render_not_black"] = mean > 0.02 or (sc.render.film_transparent and alpha > 0.01)
    report["failed"] = [k for k, v in checks.items() if not v]
    with open(os.path.join(a.out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    print("EDGESLICER_TEST_REPORT " + json.dumps({"failed": report["failed"], "checks": len(checks)}))
    return len(report["failed"])


try:
    code = main()
except Exception:
    traceback.print_exc()
    code = 100
sys.stdout.flush()
# Blender's exit code: the number of failed checks.
os._exit(code)

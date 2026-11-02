# SPDX-License-Identifier: GPL-3.0-or-later
#
# EdgeSlicer's "Open in Blender" (tests/design_showcase_realism.md 5.4) runs
#
#     blender --python <EdgeSlicer resources>/blender/edgeslicer_launch.py -- --package <folder or zip>
#
# This script puts the add-on shipped with EdgeSlicer (edgeslicer_render, next to it) on sys.path and
# registers it for this Blender session only: nothing is installed and Blender's preferences are not
# touched. Then it imports the package and shows the EdgeSlicer panel. The panel offers "Install
# permanently" (Blender's own extension install) for people who want the add-on without EdgeSlicer.
#
# Headless use (tests, batch renders):
#
#     blender -b --factory-startup --python edgeslicer_launch.py -- --package <p> --render <out.png>
#             [--samples N] [--resolution-percentage P] [--quality PREVIEW|FINAL|BEST] [--report <json>]

import argparse
import json
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(prog="edgeslicer_launch.py")
    p.add_argument("--package", required=True)
    p.add_argument("--render")
    p.add_argument("--samples", type=int)
    p.add_argument("--resolution-percentage", type=int)
    p.add_argument("--quality", choices=["PREVIEW", "FINAL", "BEST"])
    p.add_argument("--device", choices=["AUTO", "GPU", "CPU"], default="AUTO")
    p.add_argument("--report")
    return p.parse_args(argv)


def _register_for_session():
    # The add-on is read from EdgeSlicer's installation: nothing is written there.
    sys.dont_write_bytecode = True
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import edgeslicer_render

    if not hasattr(bpy.types.Scene, "edgeslicer"):
        edgeslicer_render.register()
    return edgeslicer_render


def _clear_default_scene():
    # The factory scene's cube, camera and light would sit in the middle of the print.
    for name in ("Cube", "Camera", "Light"):
        obj = bpy.data.objects.get(name)
        if obj is not None:
            bpy.data.objects.remove(obj, do_unlink=True)


def _show_panel():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != "VIEW_3D":
                continue
            space = area.spaces.active
            space.show_region_ui = True
            try:
                for region in area.regions:
                    if region.type == "UI":
                        region.active_panel_category = "EdgeSlicer"
            except (AttributeError, TypeError):
                pass
            space.shading.type = "MATERIAL"
            # Look through the scene's camera.
            space.region_3d.view_perspective = "CAMERA"
    return None


def main():
    args = _args()
    addon = _register_for_session()
    _clear_default_scene()
    summary = addon.importer.import_package(args.package, args.quality, args.device, args.samples)
    sc = bpy.context.scene
    if args.resolution_percentage:
        sc.render.resolution_percentage = args.resolution_percentage
    if args.render:
        sc.render.filepath = os.path.abspath(args.render)
        sc.render.image_settings.file_format = "PNG"
        bpy.ops.render.render(write_still=True)
        summary["render"] = sc.render.filepath
    if args.report:
        summary["blender"] = bpy.app.version_string
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
    print("EdgeSlicer: imported %s" % json.dumps(summary, sort_keys=True))
    if not bpy.app.background:
        bpy.app.timers.register(_show_panel, first_interval=0.2)


try:
    main()
except Exception as exc:  # shown in Blender's console and, for the tests, as the exit code
    import traceback

    traceback.print_exc()
    print("EdgeSlicer: the package could not be imported: %s" % exc)
    if bpy.app.background:
        sys.exit(1)

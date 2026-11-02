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


def _installed_module():
    """The permanently installed copy of the add-on (an extension or a legacy add-on), if enabled."""
    for name in bpy.context.preferences.addons.keys():
        if name == "edgeslicer_render" or name.endswith(".edgeslicer_render"):
            mod = sys.modules.get(name)
            if mod is not None:
                return name, mod
    return None, None


def _register_for_session():
    """One copy of the add-on, never two: the installed one when it is the same version as the one
    shipped with this EdgeSlicer, else the shipped one (the installed one is switched off for this
    session only; the user's preferences are not changed)."""
    # The add-on is read from EdgeSlicer's installation: nothing is written there.
    sys.dont_write_bytecode = True
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import edgeslicer_render

    name, installed = _installed_module()
    if installed is not None:
        if getattr(installed, "ADDON_VERSION", None) == edgeslicer_render.ADDON_VERSION and hasattr(installed, "importer"):
            print("EdgeSlicer: using the installed add-on %s %s" % (name, installed.ADDON_VERSION))
            return installed
        import addon_utils

        print("EdgeSlicer: the installed add-on %s is version %s, EdgeSlicer ships %s: using EdgeSlicer's for this session"
              % (name, getattr(installed, "ADDON_VERSION", "?"), edgeslicer_render.ADDON_VERSION))
        addon_utils.disable(name, default_set=False)
    if not hasattr(bpy.types.Scene, "edgeslicer"):
        edgeslicer_render.register()
    return edgeslicer_render


def _clear_default_scene():
    # The factory scene's cube, camera and light would sit in the middle of the print.
    for name in ("Cube", "Camera", "Light"):
        obj = bpy.data.objects.get(name)
        if obj is not None:
            bpy.data.objects.remove(obj, do_unlink=True)


def _views():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                yield window, area


def _show_panel():
    for _window, area in _views():
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
        if bpy.context.scene.camera is not None:
            space.region_3d.view_perspective = "CAMERA"
        area.tag_redraw()
    return None


def _popup(message):
    """An error the user sees (not only in the console)."""
    def draw(menu, _context):
        for line in message.splitlines() or [message]:
            menu.layout.label(text=line)
    for window, _area in _views():
        try:
            with bpy.context.temp_override(window=window):
                bpy.context.window_manager.popup_menu(draw, title="EdgeSlicer: the package was not imported", icon="ERROR")
            return
        except Exception:  # noqa: BLE001 - fall back below
            pass
    bpy.context.window_manager.popup_menu(draw, title="EdgeSlicer: the package was not imported", icon="ERROR")


def _import(args):
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
    return summary


def _import_in_ui(args):
    """Open in Blender: once the window is up, import, look through the camera, show the panel; any
    failure is shown in a popup."""
    try:
        _import(args)
    except Exception as exc:  # noqa: BLE001 - shown to the user
        import traceback

        traceback.print_exc()
        _popup("%s\n\nPackage: %s" % (exc, args.package))
    _show_panel()
    return None


def main():
    args = _args()
    print("EdgeSlicer: package %s" % args.package)
    if bpy.app.background:
        try:
            _import(args)
        except Exception as exc:  # the tests read the exit code
            import traceback

            traceback.print_exc()
            print("EdgeSlicer: the package could not be imported: %s" % exc)
            sys.exit(1)
    else:
        bpy.app.timers.register(lambda: _import_in_ui(args), first_interval=0.5)


main()

# SPDX-License-Identifier: GPL-3.0-or-later
# EdgeSlicer Render - the one panel ("Render like EdgeSlicer", 3D view sidebar > EdgeSlicer), its
# operators and File > Import > EdgeSlicer Showcase package (design 5.3).

import os
import subprocess
import sys
import tempfile
import zipfile

import bpy
from bpy.props import BoolProperty, EnumProperty, PointerProperty, StringProperty
from bpy.types import Operator, Panel, PropertyGroup

from . import compat, importer, package, scene

ADDON_ID = "edgeslicer_render"

LOOKS = {
    "PACKAGE": None,
    "STUDIO_WHITE": {"background": {"type": "radial_gradient", "color1": [1.0, 1.0, 1.0], "color2": [0.69, 0.71, 0.75]},
                     "floor": {"type": "shadow_catcher"}},
    "DARK_GRADIENT": {"background": {"type": "vertical_gradient", "color1": [0.05, 0.055, 0.065], "color2": [0.004, 0.004, 0.005]},
                      "floor": {"type": "shadow_catcher"}},
    "TRANSPARENT": {"background": {"type": "transparent"}, "floor": {"type": "shadow_catcher"}},
}


class EdgeSlicerSettings(PropertyGroup):
    package_path: StringProperty(name="Package", subtype="FILE_PATH",
                                 description="The EdgeSlicer Blender package (its folder, showcase.json or the zip)")
    look: EnumProperty(name="Look", items=[
        ("PACKAGE", "As exported", "The background and floor the Showcase scene has"),
        ("STUDIO_WHITE", "Studio white", "A white studio gradient with a shadow catcher"),
        ("DARK_GRADIENT", "Dark gradient", "A dark backdrop with a shadow catcher"),
        ("TRANSPARENT", "Transparent", "No backdrop: a PNG with alpha and the shadow"),
    ], default="PACKAGE")
    quality: EnumProperty(name="Quality", items=[
        ("PREVIEW", "Preview", "64 samples, adaptive, denoised"),
        ("FINAL", "Final", "512 samples, adaptive, denoised"),
        ("BEST", "Best", "2048 samples, denoised"),
    ], default="FINAL")
    device: EnumProperty(name="Device", items=[
        ("AUTO", "Auto", "The GPU set up in Blender's Preferences > System, else the CPU"),
        ("GPU", "GPU", "The GPU set up in Blender's Preferences > System"),
        ("CPU", "CPU", "The processor"),
    ], default="AUTO")
    imperfections: BoolProperty(name="Imperfections", default=True,
                                description="The slight colour variation and layer lines of a real print")
    show_advanced: BoolProperty(name="Advanced", default=False)
    status: StringProperty(default="")


def _settings(context):
    return context.scene.edgeslicer


def _do_import(context, path):
    st = _settings(context)
    look = LOOKS.get(st.look)
    summary = importer.import_package(path, st.quality, st.device, None, st.imperfections, look)
    st.status = "%d objects, %d materials%s" % (summary["objects"], summary["materials"],
                                               ", %d frames" % summary["frames"] if summary["frames"] else "")
    return summary


class EDGESLICER_OT_import(Operator):
    """Import an EdgeSlicer Showcase package and set up a Cycles scene from it"""
    bl_idname = "edgeslicer.import_package"
    bl_label = "EdgeSlicer Showcase package"
    bl_options = {"REGISTER", "UNDO"}

    filepath: StringProperty(subtype="FILE_PATH")
    filter_glob: StringProperty(default="*.zip;*.edgeshow;showcase.json", options={"HIDDEN"})

    def execute(self, context):
        try:
            _do_import(context, self.filepath)
        except (package.PackageError, compat.MissingAddon, OSError, ValueError, RuntimeError) as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}


class EDGESLICER_OT_reimport(Operator):
    """Build the scene again from the package (after EdgeSlicer wrote it again, or to apply the settings)"""
    bl_idname = "edgeslicer.reimport"
    bl_label = "Re-import / Update"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        path = bpy.path.abspath(_settings(context).package_path)
        try:
            _do_import(context, path)
        except (package.PackageError, compat.MissingAddon, OSError, ValueError, RuntimeError) as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}


class EDGESLICER_OT_render(Operator):
    """Render the Showcase still (or the video) into the package's renders folder"""
    bl_idname = "edgeslicer.render"
    bl_label = "Render"

    animation: BoolProperty(default=False)

    def execute(self, context):
        sc = context.scene
        st = _settings(context)
        if bpy.data.collections.get(scene.COLLECTION) is None or sc.camera is None:
            self.report({"ERROR"}, "No EdgeSlicer scene imported - use Re-import / Update or Open in Blender")
            return {"CANCELLED"}
        try:
            cy = compat.cycles(sc)
        except compat.MissingAddon as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        if self.animation and bpy.data.objects.get(scene.VIDEO_CAMERA) is None:
            self.report({"ERROR"}, "This package has no video: use Render image")
            return {"CANCELLED"}
        scene.use_mode(sc, video=self.animation)
        cy.samples = scene.QUALITY_SAMPLES.get(st.quality, 512)
        cy.device = "GPU" if st.device == "GPU" or (st.device == "AUTO" and compat.gpu_device_available()) else "CPU"
        out_dir = _output_dir(context)
        try:
            if self.animation:
                sc.render.filepath = os.path.join(out_dir, "frame_")
                bpy.ops.render.render("INVOKE_DEFAULT", animation=True)
            else:
                sc.render.filepath = os.path.join(out_dir, "still.png")
                bpy.ops.render.render("INVOKE_DEFAULT", write_still=True)
        except RuntimeError as exc:
            self.report({"ERROR"}, str(exc).splitlines()[-1] if str(exc) else "The render could not start")
            return {"CANCELLED"}
        return {"FINISHED"}


def _output_dir(context):
    st = _settings(context)
    try:
        pkg = package.load(bpy.path.abspath(st.package_path))
        return scene.output_dir(pkg)
    except Exception:
        path = os.path.join(tempfile.gettempdir(), "edgeslicer_renders")
        os.makedirs(path, exist_ok=True)
        return path


class EDGESLICER_OT_open_output(Operator):
    """Open the folder the renders are written to"""
    bl_idname = "edgeslicer.open_output"
    bl_label = "Open output folder"

    def execute(self, context):
        path = _output_dir(context)
        if sys.platform.startswith("win"):
            os.startfile(path)  # noqa: S606 - the user's own folder
        elif sys.platform == "darwin":
            subprocess.Popen(["open", path])
        else:
            subprocess.Popen(["xdg-open", path])
        return {"FINISHED"}


def _addon_folder():
    return os.path.dirname(os.path.abspath(__file__))


def build_addon_zip(target):
    """The add-on as an installable zip (one top folder, caches left out)."""
    src = _addon_folder()
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
        for base, dirs, files in os.walk(src):
            dirs[:] = sorted(d for d in dirs if d != "__pycache__")
            for name in sorted(files):
                if name.endswith(".pyc"):
                    continue
                full = os.path.join(base, name)
                z.write(full, os.path.join(ADDON_ID, os.path.relpath(full, src)))
    return target


class EDGESLICER_OT_install(Operator):
    """Install the EdgeSlicer add-on into Blender permanently (Blender's own extension install), so it is there without EdgeSlicer"""
    bl_idname = "edgeslicer.install_permanently"
    bl_label = "Install permanently"

    def execute(self, context):
        zip_path = build_addon_zip(os.path.join(tempfile.gettempdir(), "edgeslicer_render.zip"))
        try:
            if bpy.app.version >= (4, 2, 0) and hasattr(bpy.ops.extensions, "package_install_files"):
                bpy.ops.extensions.package_install_files(filepath=zip_path, repo="user_default", enable_on_install=True)
            else:
                bpy.ops.preferences.addon_install(filepath=zip_path, overwrite=True)
                bpy.ops.preferences.addon_enable(module=ADDON_ID)
        except Exception as exc:
            self.report({"ERROR"}, "Could not install: %s" % exc)
            return {"CANCELLED"}
        self.report({"INFO"}, "The EdgeSlicer add-on is installed.")
        return {"FINISHED"}


class EDGESLICER_PT_panel(Panel):
    bl_label = "Render like EdgeSlicer"
    bl_idname = "EDGESLICER_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "EdgeSlicer"

    def draw(self, context):
        layout = self.layout
        st = _settings(context)
        layout.prop(st, "package_path", text="")
        row = layout.row(align=True)
        row.operator(EDGESLICER_OT_reimport.bl_idname, icon="FILE_REFRESH")
        row.operator(EDGESLICER_OT_import.bl_idname, text="", icon="FILEBROWSER")
        col = layout.column()
        col.prop(st, "look")
        col.prop(st, "quality")
        col.prop(st, "device")
        col.prop(st, "imperfections")
        if st.device != "CPU" and not compat.gpu_device_available():
            col.label(text="No GPU set up: rendering on the CPU (slow).", icon="INFO")
        layout.separator()
        layout.operator(EDGESLICER_OT_render.bl_idname, text="Render image", icon="RENDER_STILL").animation = False
        row = layout.row()
        row.enabled = bpy.data.objects.get(scene.VIDEO_CAMERA) is not None
        row.operator(EDGESLICER_OT_render.bl_idname, text="Render animation", icon="RENDER_ANIMATION").animation = True
        layout.operator(EDGESLICER_OT_open_output.bl_idname, icon="FILE_FOLDER")
        if st.status:
            layout.label(text=st.status)
        if not compat.addon_installed(ADDON_ID):
            box = layout.box()
            box.label(text="Loaded for this session only.")
            box.operator(EDGESLICER_OT_install.bl_idname, icon="IMPORT")
        layout.prop(st, "show_advanced", icon="TRIA_DOWN" if st.show_advanced else "TRIA_RIGHT", emboss=False)
        if st.show_advanced:
            box = layout.box()
            box.label(text="Fine-tune in Blender's own panels:")
            box.label(text="Render: samples, denoising, light paths")
            box.label(text="Output: size, frame range, file format")
            box.label(text="World / Material: the node setups")


def _menu_import(self, context):
    self.layout.operator(EDGESLICER_OT_import.bl_idname, text="EdgeSlicer Showcase package (.zip)")


CLASSES = (EdgeSlicerSettings, EDGESLICER_OT_import, EDGESLICER_OT_reimport, EDGESLICER_OT_render,
           EDGESLICER_OT_open_output, EDGESLICER_OT_install, EDGESLICER_PT_panel)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.edgeslicer = PointerProperty(type=EdgeSlicerSettings)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    del bpy.types.Scene.edgeslicer
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)

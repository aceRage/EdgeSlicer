# SPDX-License-Identifier: GPL-3.0-or-later
# EdgeSlicer Render - importing a package into a ready-to-render Cycles scene (design 5.3 item 1).
#
#   1. bpy.ops.import_scene.gltf on scene.glb, into the "EdgeSlicer Showcase" collection;
#   2. each part's glTF material replaced by the library's setup for its finish (materials.py); a
#      material whose finish is unknown keeps the glTF one;
#   3. world, lights, camera, floor, render settings, the video's path and the overlay (scene.py).
#
# Importing again (Re-import / Update) removes what the last import made first.

import os

import bpy

from . import compat, materials, package, scene

PROP_PACKAGE = "edgeslicer_package"


def _strip_suffix(name):
    """"Name.001" -> "Name" (Blender's duplicate suffix)."""
    base, dot, num = name.rpartition(".")
    return base if dot and num.isdigit() and len(num) == 3 else name


def clear_previous():
    """Removes the objects, the camera, lights and world of the last import."""
    col = bpy.data.collections.get(scene.COLLECTION)
    if col is not None:
        for obj in list(col.all_objects):
            data = obj.data
            bpy.data.objects.remove(obj, do_unlink=True)
            if data is not None and data.users == 0:
                try:
                    if isinstance(data, bpy.types.Mesh):
                        bpy.data.meshes.remove(data)
                    elif isinstance(data, bpy.types.Camera):
                        bpy.data.cameras.remove(data)
                    elif isinstance(data, bpy.types.Light):
                        bpy.data.lights.remove(data)
                except ReferenceError:
                    pass
        for child in list(col.children):
            bpy.data.collections.remove(child)
        bpy.data.collections.remove(col)
    # The build-up's keyed height (scene.BUILD_PROP), when the scene has no other animation.
    sc = bpy.context.scene
    if scene.BUILD_PROP in sc.keys():
        if all(fc.data_path == '["%s"]' % scene.BUILD_PROP for fc in compat.action_fcurves(sc)):
            sc.animation_data_clear()
        del sc[scene.BUILD_PROP]
    if scene.BUILD_ON in sc.keys():
        del sc[scene.BUILD_ON]


def _import_glb(path, collection):
    compat.ensure_gltf_importer()
    before = set(bpy.data.objects)
    # New objects go to the active collection: make ours active for the import.
    view_layer = bpy.context.view_layer
    layer_col = _find_layer_collection(view_layer.layer_collection, collection.name)
    previous = view_layer.active_layer_collection
    if layer_col is not None:
        view_layer.active_layer_collection = layer_col
    kwargs = {"filepath": path}
    props = {p.identifier for p in bpy.ops.import_scene.gltf.get_rna_type().properties}
    if "merge_vertices" in props:
        kwargs["merge_vertices"] = False
    if "import_shading" in props:
        kwargs["import_shading"] = "NORMALS"
    if "import_scene_as_collection" in props:
        kwargs["import_scene_as_collection"] = False
    try:
        result = bpy.ops.import_scene.gltf(**kwargs)
    finally:
        if layer_col is not None:
            view_layer.active_layer_collection = previous
    if "FINISHED" not in result:
        raise package.PackageError("Blender's glTF importer could not read %s" % path)
    made = [o for o in bpy.data.objects if o not in before]
    for obj in made:
        if collection not in obj.users_collection:
            scene.link(obj, collection)
    return made


def _find_layer_collection(layer_col, name):
    if layer_col.collection.name == name:
        return layer_col
    for child in layer_col.children:
        found = _find_layer_collection(child, name)
        if found is not None:
            return found
    return None


def _node_info(obj, objects_by_name):
    """The showcase.json "objects" entry of an imported object (by the glTF node's name)."""
    for key in (obj.name, _strip_suffix(obj.name)):
        if key in objects_by_name:
            return objects_by_name[key]
    extras = obj.get("edgeslicer")
    if extras is not None:
        try:
            return {"kind": extras.get("kind"), "filament": extras.get("filament"), "layers": extras.get("layers", {}).to_dict()}
        except AttributeError:
            pass
    return None


def apply_materials(pkg, model_objects, imperfections=True, layer_bump=True):
    build_clip = any(f.get("build_height") is not None for f in pkg.frames)
    specs = pkg.get("materials", default={})
    objects_by_name = {o.get("node"): o for o in pkg.get("objects", default=[])}
    imperfect = float(pkg.get("look", "materials", "imperfections", "colour", default=0.0) or 0.0) if imperfections else 0.0
    bump = layer_bump and bool(pkg.get("look", "materials", "layer_bump", default=True))
    made = {}
    for obj in model_objects:
        if obj.type != "MESH":
            continue
        info = _node_info(obj, objects_by_name) or {}
        mat_name = info.get("material")
        if not mat_name and obj.data.materials:
            mat_name = _strip_suffix(obj.data.materials[0].name) if obj.data.materials[0] else None
        spec = specs.get(mat_name) if mat_name else None
        if spec is None:
            continue   # unknown: the glTF material stays (design 5.3)
        has_attr = obj.data.attributes.get(materials.LINEAR_ATTR) is not None
        layers = info.get("layers") or {}
        key = (mat_name, has_attr, repr(sorted(layers.items())) if layers else "")
        mat = made.get(key)
        if mat is None:
            name = mat_name if len(made) == 0 or not any(k[0] == mat_name for k in made) else "%s (%d)" % (mat_name, len(made))
            mat = materials.build_material(name, spec, layers, has_attr, imperfect, bump, build_clip)
            made[key] = mat
        obj.data.materials.clear()
        obj.data.materials.append(mat)
        obj["edgeslicer_kind"] = info.get("kind", "part")
        obj["edgeslicer_filament"] = int(info.get("filament", 1) or 1)
    return list(made.values())


def import_package(path, quality=None, device="AUTO", samples=None, imperfections=True, look_override=None):
    """Builds the scene from the package at `path` (folder, zip, or a file in the folder). Returns a
    summary dict for the panel and the tests."""
    sc = bpy.context.scene
    settings = getattr(sc, "edgeslicer", None)
    if settings is not None:
        # The panel shows the package even when a later step fails, so Re-import can try again.
        settings.package_path = path
    pkg = package.load(path)
    clear_previous()
    collection = bpy.data.collections.new(scene.COLLECTION)
    sc.collection.children.link(collection)

    quality = scene.apply_render_settings(pkg, sc, quality, device, samples)
    glb = os.path.join(pkg.root, pkg.get("glb", default="scene.glb"))
    imported = _import_glb(glb, collection)
    model_objects = [o for o in imported if o.type == "MESH"]
    # Our package bakes positions in world space: the importer's nodes are identity. Empties it made
    # (none expected) are left in place.
    mats = apply_materials(pkg, model_objects, imperfections)
    scene.build_world(pkg, sc, look_override)
    lights = scene.build_lights(pkg, collection)
    cam = scene.build_camera(pkg, sc, collection)
    floor = scene.build_floor(pkg, collection, look_override)
    video_cam = scene.build_animation(pkg, sc, cam, model_objects, collection)
    scene.use_mode(sc, video=False)
    overlay = scene.build_overlay(pkg, sc)
    sc.render.filepath = os.path.join(scene.output_dir(pkg), "frame_")
    sc[PROP_PACKAGE] = pkg.source
    if settings is not None:
        settings.package_path = pkg.source
        settings.quality = quality
    return {
        "root": pkg.root,
        "objects": len(model_objects),
        "materials": len(mats),
        "lights": len(lights),
        "floor": len(floor),
        "frames": len(pkg.frames),
        "overlay": overlay,
        "camera": cam.name,
        "video_camera": video_cam.name if video_cam is not None else None,
        "view_transform": sc.view_settings.view_transform,
        "has_silk_attr": any(o.data.attributes.get(materials.LINEAR_ATTR) is not None for o in model_objects),
    }

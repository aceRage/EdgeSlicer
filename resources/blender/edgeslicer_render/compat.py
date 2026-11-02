# SPDX-License-Identifier: GPL-3.0-or-later
# EdgeSlicer Render - Blender API differences between versions, in one place (design 5.5).
#
# Blender 4.0 renamed the Principled BSDF's inputs; 4.4 / 5.0 moved an action's F-curves into
# channel bags; 5.0 replaced the scene's compositor node tree with a compositor node group. Every
# version-dependent call the add-on makes goes through here.

import bpy

VERSION = bpy.app.version

# Principled BSDF inputs: the 4.x name -> the 3.x name (None: no such input there).
_PRINCIPLED_3X = {
    "Subsurface Weight": "Subsurface",
    "Specular IOR Level": "Specular",
    "Transmission Weight": "Transmission",
    "Coat Weight": "Clearcoat",
    "Coat Roughness": "Clearcoat Roughness",
    "Sheen Weight": "Sheen",
    "Sheen Roughness": None,
    "Emission Color": "Emission",
    "Subsurface Scale": None,
}


def principled_input(node, name):
    """The Principled BSDF socket called `name` (4.x naming) on any supported version, or None."""
    sock = node.inputs.get(name)
    if sock is not None:
        return sock
    old = _PRINCIPLED_3X.get(name, name)
    return node.inputs.get(old) if old else None


def set_input(node, name, value):
    """Sets an input's default value if the socket exists; the 3.x Specular Tint is a float."""
    sock = principled_input(node, name) if node.bl_idname == "ShaderNodeBsdfPrincipled" else node.inputs.get(name)
    if sock is None:
        return None
    try:
        if name == "Specular Tint" and sock.type == "VALUE" and not isinstance(value, (int, float)):
            value = 0.0
        if name == "Emission Color" and sock.type == "RGBA" and len(value) == 3:
            value = (value[0], value[1], value[2], 1.0)
        sock.default_value = value
    except (TypeError, ValueError, AttributeError):
        try:
            sock.default_value = tuple(value) + (1.0,)
        except Exception:
            return None
    return sock


def enable_nodes(idblock):
    """Materials, worlds and lights always use nodes from 5.0; before that they must be switched on."""
    try:
        if hasattr(idblock, "use_nodes") and not idblock.use_nodes:
            idblock.use_nodes = True
    except Exception:
        pass


def set_shadow_catcher(obj, on=True):
    try:
        obj.is_shadow_catcher = on
    except AttributeError:
        obj.cycles.is_shadow_catcher = on


def set_ray_visibility(obj, camera=True, diffuse=True, glossy=True, transmission=True, volume=True, shadow=True):
    try:
        obj.visible_camera = camera
        obj.visible_diffuse = diffuse
        obj.visible_glossy = glossy
        obj.visible_transmission = transmission
        obj.visible_volume_scatter = volume
        obj.visible_shadow = shadow
    except AttributeError:
        vis = obj.cycles_visibility
        vis.camera, vis.diffuse, vis.glossy, vis.transmission, vis.scatter, vis.shadow = camera, diffuse, glossy, transmission, volume, shadow


def set_view_transform(scene, candidates):
    """The first view transform of `candidates` this Blender has; returns its name."""
    for name in candidates:
        try:
            scene.view_settings.view_transform = name
            return name
        except TypeError:
            continue
    return scene.view_settings.view_transform


def set_look_none(scene):
    for name in ("None", "AgX - Base Contrast"):
        try:
            scene.view_settings.look = name
            return
        except TypeError:
            continue


def compositor_tree(scene, create=True):
    """The scene's compositor node tree and its output node: (tree, output) or (None, None).
    Blender 5.0+: a compositor node group with a Group Output; before: scene.node_tree."""
    if hasattr(scene, "compositing_node_group"):
        tree = scene.compositing_node_group
        if tree is None and create:
            tree = bpy.data.node_groups.new("EdgeSlicer Compositor", "CompositorNodeTree")
            tree.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
            scene.compositing_node_group = tree
        if tree is None:
            return None, None
        out = next((n for n in tree.nodes if n.bl_idname == "NodeGroupOutput"), None)
        if out is None and create:
            out = tree.nodes.new("NodeGroupOutput")
        return tree, out
    if create:
        scene.use_nodes = True
    tree = getattr(scene, "node_tree", None)
    if tree is None:
        return None, None
    out = next((n for n in tree.nodes if n.bl_idname == "CompositorNodeComposite"), None)
    if out is None and create:
        out = tree.nodes.new("CompositorNodeComposite")
    return tree, out


def action_fcurves(id_data):
    """Every F-curve animating `id_data` (legacy actions and layered / slotted ones)."""
    ad = getattr(id_data, "animation_data", None)
    if ad is None or ad.action is None:
        return []
    action = ad.action
    if hasattr(action, "fcurves"):
        try:
            return list(action.fcurves)
        except Exception:
            pass
    try:
        from bpy_extras import anim_utils

        bag = anim_utils.action_get_channelbag_for_slot(action, ad.action_slot)
        return list(bag.fcurves) if bag is not None else []
    except Exception:
        pass
    curves = []
    for layer in getattr(action, "layers", []):
        for strip in layer.strips:
            for bag in getattr(strip, "channelbags", []):
                curves.extend(bag.fcurves)
    return curves


def make_linear(id_data):
    for fc in action_fcurves(id_data):
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"


def gpu_device_available():
    """True when the user's Cycles preferences already name a GPU backend with a device switched on.
    The preferences are only read: changing them would be saved into the user's Blender config."""
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
    except (KeyError, AttributeError):
        return False
    if getattr(prefs, "compute_device_type", "NONE") in ("NONE", ""):
        return False
    # No refresh_devices(): it can add device entries and mark the preferences as changed, and Blender
    # auto-saves changed preferences on quit. The stored list is what the user set up.
    return any(d.use and d.type != "CPU" for d in getattr(prefs, "devices", []))


def addon_installed(module_name):
    """Whether the add-on is installed permanently (as an extension or a legacy add-on)."""
    import addon_utils

    for mod in addon_utils.modules(refresh=False):
        name = getattr(mod, "__name__", "")
        if name == module_name or name.endswith("." + module_name):
            return True
    return False

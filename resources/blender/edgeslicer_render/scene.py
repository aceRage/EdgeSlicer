# SPDX-License-Identifier: GPL-3.0-or-later
# EdgeSlicer Render - the scene around the parts (design 5.3 items 3, 4, 6, 7): the world (HDRI with
# rotation and strength for the light; the backdrop, gradient or photo for the camera), the lights,
# the camera with its depth of field, the floor or shadow catcher, the ground-projected dome, the
# Cycles settings, colour management, the video's camera path and the stills' overlay.

import math
import os

import bmesh
import bpy
from mathutils import Matrix, Vector

from . import compat, materials

COLLECTION = "EdgeSlicer Showcase"
QUALITY_SAMPLES = {"PREVIEW": 64, "FINAL": 512, "BEST": 2048}
BUILD_PROP = "edgeslicer_build_height"
BUILD_ON = "edgeslicer_build_on"
STILL_CAMERA = "EdgeSlicer Camera"
VIDEO_CAMERA = "EdgeSlicer Video Camera"
QUALITY_FROM_PACKAGE = {"draft": "PREVIEW", "high": "FINAL", "best": "BEST"}


def _vec(v, default=(0.0, 0.0, 0.0)):
    return Vector(v if v else default)


def _rgba(c):
    return (float(c[0]), float(c[1]), float(c[2]), 1.0)


def link(obj, collection):
    for c in list(obj.users_collection):
        c.objects.unlink(obj)
    collection.objects.link(obj)


def look_at_matrix(location, target, up):
    loc, tgt = Vector(location), Vector(target)
    fwd = (tgt - loc)
    if fwd.length < 1e-9:
        fwd = Vector((0.0, 1.0, 0.0))
    fwd.normalize()
    upv = Vector(up) if up else Vector((0.0, 0.0, 1.0))
    right = fwd.cross(upv)
    if right.length < 1e-9:
        right = fwd.cross(Vector((0.0, 1.0, 0.0)))
    right.normalize()
    cam_up = right.cross(fwd)
    m = Matrix((right, cam_up, -fwd)).transposed().to_4x4()
    m.translation = loc
    return m


# --- world --------------------------------------------------------------------------------------

def _env_texture(tree, pkg, rotation_deg, pitch_deg=0.0):
    """Environment texture node reading the package's HDRI, turned like Showcase turns it: the
    panorama turns by +rotation about Z (Showcase looks up the direction turned by -rotation). A
    backdrop pitch (camera rays only) first tilts the looked-up direction up about the camera's
    horizontal axis, so more sky shows (backdrop_pitch_matrix() in ShowcaseLens)."""
    path = pkg.path(pkg.get("environment", "file"))
    if not path:
        return None
    nodes, links = tree.nodes, tree.links
    tc = nodes.new("ShaderNodeTexCoord")
    mapping = nodes.new("ShaderNodeMapping")
    mapping.vector_type = "VECTOR"
    rot = Matrix.Rotation(-math.radians(rotation_deg), 3, "Z")
    if abs(pitch_deg) > 1e-6:
        cam = pkg.get("camera", default={})
        fwd = _vec(cam.get("target")) - _vec(cam.get("location"), (0, -1, 0))
        right = fwd.cross(Vector((0.0, 0.0, 1.0)))
        if right.length > 1e-9:
            rot = rot @ Matrix.Rotation(math.radians(pitch_deg), 3, right.normalized())
    mapping.inputs["Rotation"].default_value = rot.to_euler("XYZ")
    links.new(tc.outputs["Generated"], mapping.inputs["Vector"])
    env = nodes.new("ShaderNodeTexEnvironment")
    env.image = bpy.data.images.load(path, check_existing=True)
    links.new(mapping.outputs["Vector"], env.inputs["Vector"])
    return env


def _window_image(tree, pkg, rel, mapping_xy):
    """A picture filling the frame by the camera's window coordinates: uv = f * scale + offset."""
    path = pkg.path(rel)
    if not path:
        return None
    nodes, links = tree.nodes, tree.links
    tc = nodes.new("ShaderNodeTexCoord")
    mp = nodes.new("ShaderNodeMapping")
    mp.vector_type = "POINT"
    sx, sy, ox, oy = mapping_xy
    mp.inputs["Scale"].default_value = (sx, sy, 1.0)
    mp.inputs["Location"].default_value = (ox, oy, 0.0)
    links.new(tc.outputs["Window"], mp.inputs["Vector"])
    img = nodes.new("ShaderNodeTexImage")
    img.image = bpy.data.images.load(path, check_existing=True)
    img.extension = "EXTEND"
    links.new(mp.outputs["Vector"], img.inputs["Vector"])
    return img


def _cover_mapping(image, frame_aspect):
    """Zoom 1 of plate_mapping(): the picture covers the frame, cropped to it."""
    if image is None or image.size[1] == 0:
        return (1.0, 1.0, 0.0, 0.0)
    ia = image.size[0] / image.size[1]
    if ia > frame_aspect:
        sx = frame_aspect / ia
        return (sx, 1.0, 0.5 * (1.0 - sx), 0.0)
    sy = ia / frame_aspect
    return (1.0, sy, 0.0, 0.5 * (1.0 - sy))


def build_world(pkg, scene, look_override=None):
    world = bpy.data.worlds.get("EdgeSlicer World") or bpy.data.worlds.new("EdgeSlicer World")
    compat.enable_nodes(world)
    scene.world = world
    tree = world.node_tree
    tree.nodes.clear()
    nodes, links = tree.nodes, tree.links
    out = nodes.new("ShaderNodeOutputWorld")

    env_cfg = pkg.get("environment", default={})
    bg_cfg = dict(pkg.get("background", default={}))
    if look_override:
        bg_cfg.update(look_override.get("background", {}))
    rotation = float(env_cfg.get("rotation_deg", 0.0) or 0.0)
    strength = float(env_cfg.get("strength", 1.0) or 0.0)

    # The light: the HDRI, or the fallback colour.
    light = nodes.new("ShaderNodeBackground")
    light.inputs["Strength"].default_value = strength
    env = _env_texture(tree, pkg, rotation)
    if env is not None:
        links.new(env.outputs["Color"], light.inputs["Color"])
    else:
        light.inputs["Color"].default_value = _rgba(env_cfg.get("fallback_color", [0.8, 0.8, 0.8]))
        light.inputs["Strength"].default_value = 0.6 * float(env_cfg.get("intensity", 0.7) or 0.7)

    # What the camera sees.
    btype = bg_cfg.get("type", "radial_gradient")
    aspect = scene.render.resolution_x / max(1, scene.render.resolution_y)
    cam_bg = nodes.new("ShaderNodeBackground")
    cam_bg.inputs["Strength"].default_value = 1.0
    camera_color = None
    if btype in ("environment",) and env is not None:
        pitch = float(bg_cfg.get("backdrop_pitch", 0.0) or 0.0)
        env_cam = _env_texture(tree, pkg, rotation, pitch) if abs(pitch) > 1e-3 else env
        camera_color = env_cam.outputs["Color"]
        cam_bg.inputs["Strength"].default_value = strength
    elif btype == "backplate" and bg_cfg.get("image"):
        plate = bg_cfg.get("plate") or {}
        img = _window_image(tree, pkg, bg_cfg["image"], (plate.get("sx", 1.0), plate.get("sy", 1.0), plate.get("ox", 0.0), plate.get("oy", 0.0)))
        camera_color = img.outputs["Color"] if img else None
    elif btype == "image" and bg_cfg.get("image"):
        path = pkg.path(bg_cfg["image"])
        image = bpy.data.images.load(path, check_existing=True) if path else None
        img = _window_image(tree, pkg, bg_cfg["image"], _cover_mapping(image, aspect))
        camera_color = img.outputs["Color"] if img else None
    elif btype in ("vertical_gradient", "radial_gradient"):
        tc = nodes.new("ShaderNodeTexCoord")
        sep = nodes.new("ShaderNodeSeparateXYZ")
        links.new(tc.outputs["Window"], sep.inputs[0])
        ramp_in = sep.outputs["Y"]
        if btype == "radial_gradient":
            # Distance from the frame's centre, 1 at the corners.
            dx = nodes.new("ShaderNodeMath"); dx.operation = "SUBTRACT"; dx.inputs[1].default_value = 0.5
            dy = nodes.new("ShaderNodeMath"); dy.operation = "SUBTRACT"; dy.inputs[1].default_value = 0.5
            links.new(sep.outputs["X"], dx.inputs[0]); links.new(sep.outputs["Y"], dy.inputs[0])
            ax = nodes.new("ShaderNodeMath"); ax.operation = "MULTIPLY"; ax.inputs[1].default_value = aspect
            links.new(dx.outputs[0], ax.inputs[0])
            comb = nodes.new("ShaderNodeCombineXYZ")
            links.new(ax.outputs[0], comb.inputs[0]); links.new(dy.outputs[0], comb.inputs[1])
            ln = nodes.new("ShaderNodeVectorMath"); ln.operation = "LENGTH"
            links.new(comb.outputs[0], ln.inputs[0])
            norm = nodes.new("ShaderNodeMath"); norm.operation = "DIVIDE"
            norm.inputs[1].default_value = 0.5 * math.hypot(aspect, 1.0)
            links.new(ln.outputs["Value"], norm.inputs[0])
            ramp_in = norm.outputs[0]
        mix = nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        mix.clamp_factor = True
        links.new(ramp_in, mix.inputs[0])
        c1, c2 = bg_cfg.get("color1", [1, 1, 1]), bg_cfg.get("color2", [0.7, 0.7, 0.7])
        if btype == "vertical_gradient":   # color1 at the top (y = 1)
            mix.inputs[6].default_value, mix.inputs[7].default_value = _rgba(c2), _rgba(c1)
        else:                              # color1 at the centre
            mix.inputs[6].default_value, mix.inputs[7].default_value = _rgba(c1), _rgba(c2)
        camera_color = mix.outputs[2]
    else:   # solid, transparent (the film is transparent then), or an environment that is missing
        cam_bg.inputs["Color"].default_value = _rgba(bg_cfg.get("color1", [1, 1, 1]))
    if camera_color is not None:
        links.new(camera_color, cam_bg.inputs["Color"])

    lp = nodes.new("ShaderNodeLightPath")
    mix = nodes.new("ShaderNodeMixShader")
    links.new(lp.outputs["Is Camera Ray"], mix.inputs["Fac"])
    links.new(light.outputs[0], mix.inputs[1])
    links.new(cam_bg.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], out.inputs["Surface"])
    scene.render.film_transparent = btype == "transparent"
    return world


# --- lights and camera ---------------------------------------------------------------------------

def build_lights(pkg, collection):
    made = []
    for spec in pkg.get("lights", default=[]):
        name = spec.get("name", "EdgeSlicer Light")
        data = bpy.data.lights.new(name, type="SUN")
        data.energy = float(spec.get("strength", 1.0))
        data.color = tuple(spec.get("color", [1, 1, 1]))
        data.angle = math.radians(float(spec.get("angle_deg", 3.0)))
        try:
            data.use_shadow = bool(pkg.get("shadows", default=True))
        except AttributeError:
            pass
        obj = bpy.data.objects.new(name, data)
        collection.objects.link(obj)
        d = _vec(spec.get("direction"), (0, 0, 1)).normalized()
        # A sun shines along its local -Z: turn +Z onto the direction TO the light.
        obj.rotation_mode = "QUATERNION"
        obj.rotation_quaternion = Vector((0.0, 0.0, 1.0)).rotation_difference(d)
        obj.location = Vector(pkg.get("model", "center", default=[0, 0, 0])) + d * 0.5
        made.append(obj)
    return made


def apply_camera_frame(cam_obj, spec):
    cam = cam_obj.data
    cam_obj.matrix_world = look_at_matrix(spec["location"], spec["target"], spec.get("up"))
    if spec.get("lens_mm"):
        cam.lens = float(spec["lens_mm"])
    if spec.get("ortho_height") and cam.type == "ORTHO":
        cam.ortho_scale = float(spec["ortho_height"])


def build_camera(pkg, scene, collection):
    spec = pkg.get("camera", default={})
    cam = bpy.data.cameras.new("EdgeSlicer Camera")
    cam.type = "ORTHO" if spec.get("type") == "ortho" else "PERSP"
    cam.sensor_fit = "VERTICAL"
    cam.sensor_height = float(spec.get("sensor_height_mm", 24.0))
    cam.sensor_width = cam.sensor_height * max(1e-3, scene.render.resolution_x / max(1, scene.render.resolution_y))
    cam.clip_start = float(spec.get("clip_start", 0.0005))
    cam.clip_end = float(spec.get("clip_end", 2000.0))
    obj = bpy.data.objects.new("EdgeSlicer Camera", cam)
    collection.objects.link(obj)
    apply_camera_frame(obj, spec)
    dof = spec.get("dof") or {}
    cam.dof.use_dof = bool(dof.get("enabled")) and cam.type == "PERSP"
    cam.dof.aperture_fstop = float(dof.get("f_number", 2.8))
    if dof.get("focus_point"):
        focus = bpy.data.objects.new("EdgeSlicer Focus", None)
        focus.empty_display_size = 0.01
        focus.location = Vector(dof["focus_point"])
        collection.objects.link(focus)
        cam.dof.focus_object = focus
    cam.dof.focus_distance = float(dof.get("focus_distance", 0.3))
    scene.camera = obj
    return obj


# --- floor and ground dome -----------------------------------------------------------------------

def _plane(name, center, size, collection, z=0.0):
    mesh = bpy.data.meshes.new(name)
    h = 0.5 * size
    cx, cy = center[0], center[1]
    mesh.from_pydata([(cx - h, cy - h, z), (cx + h, cy - h, z), (cx + h, cy + h, z), (cx - h, cy + h, z)], [], [(0, 1, 2, 3)])
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _floor_material(pkg, floor):
    ftype = floor.get("type")
    mat = bpy.data.materials.get("EdgeSlicer Floor") or bpy.data.materials.new("EdgeSlicer Floor")
    compat.enable_nodes(mat)
    tree = mat.node_tree
    tree.nodes.clear()
    nodes, links = tree.nodes, tree.links
    out = nodes.new("ShaderNodeOutputMaterial")
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    links.new(bsdf.outputs[0], out.inputs["Surface"])
    reflection = float(floor.get("reflection", 0.0) or 0.0)
    rough = float(floor.get("roughness", 0.6) or 0.6)
    compat.set_input(bsdf, "Roughness", max(0.02, rough * (1.0 - 0.9 * reflection)))
    compat.set_input(bsdf, "Base Color", _rgba(floor.get("color", [0.9, 0.9, 0.9])))
    if ftype in ("texture", "image") and floor.get("image"):
        path = pkg.path(floor["image"])
        if path:
            tc = nodes.new("ShaderNodeTexCoord")
            mp = nodes.new("ShaderNodeMapping")
            tile = max(1e-4, float(floor.get("tile", 0.5) or 0.5))
            flip = -1.0 if floor.get("flip") else 1.0
            mp.inputs["Scale"].default_value = (flip / tile, 1.0 / tile, 1.0)
            mp.inputs["Rotation"].default_value = (0.0, 0.0, math.radians(90.0 * int(floor.get("rotation", 0) or 0)))
            links.new(tc.outputs["Object"], mp.inputs["Vector"])
            img = nodes.new("ShaderNodeTexImage")
            img.image = bpy.data.images.load(path, check_existing=True)
            img.extension = "REPEAT"
            links.new(mp.outputs["Vector"], img.inputs["Vector"])
            links.new(img.outputs["Color"], compat.principled_input(bsdf, "Base Color"))
    elif ftype == "grid":
        spacing = max(1e-4, float(floor.get("grid_spacing", 0.01) or 0.01))
        tc = nodes.new("ShaderNodeTexCoord")
        brick = nodes.new("ShaderNodeTexBrick")
        brick.offset = 0.0
        brick.squash = 1.0
        brick.inputs["Scale"].default_value = 1.0 / spacing
        brick.inputs["Mortar Size"].default_value = 0.03
        brick.inputs["Brick Width"].default_value = 1.0
        brick.inputs["Row Height"].default_value = 1.0
        base = floor.get("color", [0.9, 0.9, 0.9])
        brick.inputs["Color1"].default_value = _rgba(base)
        brick.inputs["Color2"].default_value = _rgba(base)
        brick.inputs["Mortar"].default_value = _rgba([c * 0.6 for c in base])
        links.new(tc.outputs["Object"], brick.inputs["Vector"])
        links.new(brick.outputs["Color"], compat.principled_input(bsdf, "Base Color"))
    return mat


def _dome(pkg, ground, collection):
    """The ground-projected panorama: a sphere around the HDRI's camera cut by the floor, showing the
    panorama by the direction from its centre; seen by the camera only (the world lights the scene)."""
    center = Vector(ground["center"])
    radius = float(ground["radius"])
    mesh = bpy.data.meshes.new("EdgeSlicer Ground Dome")
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=96, v_segments=48, radius=radius)
    bmesh.ops.translate(bm, verts=bm.verts, vec=center)
    geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
    cut = bmesh.ops.bisect_plane(bm, geom=geom, plane_co=(0.0, 0.0, -0.00005), plane_no=(0.0, 0.0, 1.0), clear_inner=True)
    edges = [e for e in cut["geom_cut"] if isinstance(e, bmesh.types.BMEdge)]
    if edges:
        bmesh.ops.edgenet_fill(bm, edges=edges)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bmesh.ops.reverse_faces(bm, faces=bm.faces)   # seen from inside
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new("EdgeSlicer Ground Dome", mesh)
    collection.objects.link(obj)
    mat = bpy.data.materials.get("EdgeSlicer Ground Dome") or bpy.data.materials.new("EdgeSlicer Ground Dome")
    compat.enable_nodes(mat)
    tree = mat.node_tree
    tree.nodes.clear()
    nodes, links = tree.nodes, tree.links
    out = nodes.new("ShaderNodeOutputMaterial")
    geo = nodes.new("ShaderNodeNewGeometry")
    sub = nodes.new("ShaderNodeVectorMath")
    sub.operation = "SUBTRACT"
    sub.inputs[1].default_value = center
    links.new(geo.outputs["Position"], sub.inputs[0])
    env_cfg = pkg.get("environment", default={})
    mp = nodes.new("ShaderNodeMapping")
    mp.vector_type = "VECTOR"
    mp.inputs["Rotation"].default_value = (0.0, 0.0, -math.radians(float(env_cfg.get("rotation_deg", 0.0) or 0.0)))
    links.new(sub.outputs["Vector"], mp.inputs["Vector"])
    env = nodes.new("ShaderNodeTexEnvironment")
    path = pkg.path(env_cfg.get("file"))
    if path:
        env.image = bpy.data.images.load(path, check_existing=True)
    links.new(mp.outputs["Vector"], env.inputs["Vector"])
    em = nodes.new("ShaderNodeEmission")
    em.inputs["Strength"].default_value = float(env_cfg.get("strength", 1.0) or 1.0)
    links.new(env.outputs["Color"], em.inputs["Color"])
    links.new(em.outputs[0], out.inputs["Surface"])
    mesh.materials.append(mat)
    compat.set_ray_visibility(obj, camera=True, diffuse=False, glossy=True, transmission=True, volume=False, shadow=False)
    return obj


def build_floor(pkg, collection, look_override=None):
    floor = dict(pkg.get("floor", default={}))
    if look_override:
        floor.update(look_override.get("floor", {}))
    ftype = floor.get("type", "shadow_catcher")
    made = []
    ground = pkg.get("background", "ground", default={})
    if ground.get("enabled") and pkg.get("environment", "file"):
        made.append(_dome(pkg, ground, collection))
    if ftype == "none":
        return made
    size = float(floor.get("size", 2.0) or 2.0)
    plane = _plane("EdgeSlicer Floor", floor.get("center", [0, 0, 0]), size, collection, float(floor.get("z", 0.0) or 0.0))
    if ftype == "shadow_catcher":
        compat.set_shadow_catcher(plane, True)
        plane.data.materials.append(_floor_material(pkg, dict(floor, type="solid")))
    else:
        plane.data.materials.append(_floor_material(pkg, floor))
    made.append(plane)
    return made


# --- render settings -----------------------------------------------------------------------------

def apply_render_settings(pkg, scene, quality=None, device="AUTO", samples=None):
    cy = compat.cycles(scene)
    out = pkg.get("output", default={})
    scene.render.resolution_x = int(out.get("width", 1920))
    scene.render.resolution_y = int(out.get("height", 1080))
    scene.render.resolution_percentage = 100
    scene.render.fps = int(out.get("fps", 30) or 30)
    quality = quality or QUALITY_FROM_PACKAGE.get(pkg.get("look", "quality", default="high"), "FINAL")
    cy.samples = int(samples or QUALITY_SAMPLES.get(quality, 512))
    cy.use_adaptive_sampling = quality != "BEST"
    cy.use_denoising = True
    try:
        cy.denoiser = "OPENIMAGEDENOISE"
    except TypeError:
        pass
    if device == "GPU" or (device == "AUTO" and compat.gpu_device_available()):
        cy.device = "GPU"
    else:
        cy.device = "CPU"
    # Showcase's tone curve (D-R1): Khronos PBR Neutral by default, filmic as an option.
    tone = pkg.get("look", "tone", default="neutral")
    if tone == "filmic":
        compat.set_view_transform(scene, ["Filmic", "AgX", "Standard"])
    else:
        compat.set_view_transform(scene, ["Khronos PBR Neutral", "AgX", "Standard"])
    compat.set_look_none(scene)
    scene.view_settings.exposure = float(pkg.get("look", "exposure", default=0.0) or 0.0)
    scene.view_settings.gamma = 1.0
    return quality


# --- video ---------------------------------------------------------------------------------------

def use_mode(scene, video):
    """A still renders through the still's camera with the whole print; the video through the video
    camera with its build-up. The viewport shows the still unless asked."""
    cam = bpy.data.objects.get(VIDEO_CAMERA if video else STILL_CAMERA)
    if cam is None:
        cam = bpy.data.objects.get(STILL_CAMERA)
    if cam is not None:
        scene.camera = cam
    scene[BUILD_ON] = 1.0 if video else 0.0
    if not video:
        scene.frame_set(scene.frame_start)
    return cam


def build_animation(pkg, scene, still_cam, model_objects, collection):
    """The video's camera path on its own camera (the still keeps the scene's camera), the turntable
    and the build-up. Returns the video camera, or None for a still."""
    frames = pkg.frames
    scene[BUILD_ON] = 0.0
    if not frames:
        scene.frame_start = scene.frame_end = 1
        return None
    cam_obj = still_cam.copy()
    cam_obj.data = still_cam.data.copy()
    cam_obj.name = VIDEO_CAMERA
    cam_obj.data.name = VIDEO_CAMERA
    collection.objects.link(cam_obj)
    scene.frame_start = 1
    scene.frame_end = len(frames)
    scene.render.fps = int(pkg.camera_path.get("fps", 30) or 30)
    cam_obj.rotation_mode = "QUATERNION"
    any_spin = any(abs(float(f.get("spin_deg", 0.0) or 0.0)) > 1e-6 for f in frames)
    any_build = any(f.get("build_height") is not None for f in frames)
    turntable = None
    if any_spin:
        turntable = bpy.data.objects.new("EdgeSlicer Turntable", None)
        turntable.location = Vector(pkg.camera_path.get("turntable_center", [0, 0, 0]))
        collection.objects.link(turntable)
        for obj in model_objects:
            mw = obj.matrix_world.copy()
            obj.parent = turntable
            obj.matrix_parent_inverse = turntable.matrix_world.inverted()
            obj.matrix_world = mw
    # The build-up: like Showcase's clip fallback, everything above the height is cut away per pixel.
    # The height is a scene property keyed per frame, which the materials read (Attribute node, view
    # layer lookup; materials.build_material(build_clip=True)).
    if any_build:
        scene[BUILD_PROP] = 1000.0
    for i, f in enumerate(frames, start=1):
        apply_camera_frame(cam_obj, f)
        cam_obj.keyframe_insert("location", frame=i)
        cam_obj.keyframe_insert("rotation_quaternion", frame=i)
        if f.get("lens_mm"):
            cam_obj.data.lens = float(f["lens_mm"])
            cam_obj.data.keyframe_insert("lens", frame=i)
        if cam_obj.data.type == "ORTHO" and f.get("ortho_height"):
            cam_obj.data.ortho_scale = float(f["ortho_height"])
            cam_obj.data.keyframe_insert("ortho_scale", frame=i)
        if turntable is not None:
            turntable.rotation_euler = (0.0, 0.0, math.radians(float(f.get("spin_deg", 0.0) or 0.0)))
            turntable.keyframe_insert("rotation_euler", frame=i)
        if any_build:
            h = f.get("build_height")
            scene[BUILD_PROP] = 1000.0 if h is None else float(h) + 1e-6
            scene.keyframe_insert('["%s"]' % BUILD_PROP, frame=i)
    # The keys are already eased (one per frame): linear between them.
    for idb in [cam_obj, cam_obj.data, turntable, scene if any_build else None]:
        if idb is not None:
            compat.make_linear(idb)
    scene.frame_set(1)
    return cam_obj


# --- overlay (stills) ----------------------------------------------------------------------------

def build_overlay(pkg, scene):
    """The callouts and watermark PNG over the render, in the compositor (stills only, D-R18)."""
    rel = pkg.get("overlay")
    path = pkg.path(rel) if rel else None
    if not path or pkg.frames:
        return False
    try:
        tree, out = compat.compositor_tree(scene, create=True)
        if tree is None:
            return False
        nodes, links = tree.nodes, tree.links
        rl = next((n for n in nodes if n.bl_idname == "CompositorNodeRLayers"), None) or nodes.new("CompositorNodeRLayers")
        img = nodes.new("CompositorNodeImage")
        img.image = bpy.data.images.load(path, check_existing=True)
        over = nodes.new("CompositorNodeAlphaOver")
        links.new(rl.outputs["Image"], over.inputs[1] if over.inputs[0].name == "Fac" else over.inputs["Background"])
        links.new(img.outputs["Image"], over.inputs[2] if over.inputs[0].name == "Fac" else over.inputs["Foreground"])
        links.new(over.outputs[0], out.inputs[0])
        return True
    except Exception as exc:   # the compositor API differs between versions; the render is still right
        print("EdgeSlicer: no overlay in the compositor (%s)" % exc)
        return False


def output_dir(pkg):
    path = os.path.join(pkg.root, "renders")
    os.makedirs(path, exist_ok=True)
    return path

# SPDX-License-Identifier: GPL-3.0-or-later
# EdgeSlicer Render - the material library: one Principled BSDF setup per finish (design 5.3 item 2).
#
# EdgeSlicer maps each finish to Principled inputs and a few procedural details (showcase.json
# "materials", also kept in the glTF material's extras); this module turns them into node trees:
#
#   standard / matte / gloss / satin   roughness, coat, sheen
#   silk                               anisotropic specular along the layers (radial tangent about Z on
#                                      meshes, the extrusion direction attribute _SILK_DIR on beads)
#   transparent / translucent          transmission + volume absorption, subsurface for milky plastics
#   metallic                           metallic with the filament colour
#   carbon_fiber / matte_textured      sheen + a fine fibre bump
#   sparkle                            Voronoi-cell flakes that tilt the normal and turn metallic
#   marble / wood                      stone specks / wood bands in the colour
#   (any)                              the layer lines as a bump from the object's Z (meshes), glow as
#                                      emission with the lights off, and an optional colour variation

import math

import bpy

from . import compat

LINEAR_ATTR = "_SILK_DIR"
BUILD_PROP = "edgeslicer_build_height"


class _Builder:
    def __init__(self, tree):
        self.tree = tree
        self.nodes = tree.nodes
        self.links = tree.links
        self.x = -1400

    def node(self, kind, **props):
        n = self.nodes.new(kind)
        for k, v in props.items():
            setattr(n, k, v)
        n.location = (self.x, -200 * (len(self.nodes) % 6))
        self.x += 120
        return n

    def link(self, out, inp):
        self.links.new(out, inp)

    def math(self, op, a, b=None, c=None):
        n = self.node("ShaderNodeMath", operation=op)
        for i, v in enumerate((a, b, c)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                n.inputs[i].default_value = v
            else:
                self.link(v, n.inputs[i])
        return n.outputs[0]

    def vmath(self, op, a, b=None, scale=None):
        n = self.node("ShaderNodeVectorMath", operation=op)
        for i, v in enumerate((a, b)):
            if v is None:
                continue
            if isinstance(v, (tuple, list)):
                n.inputs[i].default_value = v
            else:
                self.link(v, n.inputs[i])
        if scale is not None:
            sock = n.inputs.get("Scale")
            if isinstance(scale, (int, float)):
                sock.default_value = scale
            else:
                self.link(scale, sock)
        return n.outputs["Vector"] if op != "DOT_PRODUCT" and op != "LENGTH" else n.outputs["Value"]

    def mix_rgb(self, factor, a, b, blend="MIX"):
        n = self.node("ShaderNodeMix", data_type="RGBA", blend_type=blend)
        for sock, v in ((n.inputs[0], factor), (n.inputs[6], a), (n.inputs[7], b)):
            if isinstance(v, (int, float)):
                sock.default_value = v
            elif isinstance(v, (tuple, list)):
                sock.default_value = tuple(v) + ((1.0,) if len(v) == 3 else ())
            else:
                self.link(v, sock)
        return n.outputs[2]


def _rgba(c):
    return (float(c[0]), float(c[1]), float(c[2]), 1.0)


def _layer_profile(b, layers):
    """Height 0..1 of the bead profile at the object's Z: a half circle per layer."""
    first = float(layers.get("first_m") or 0.0)
    height = float(layers.get("height_m") or 0.0)
    tops = layers.get("tops_m") or []
    if not layers.get("uniform", True) and len(tops) > 1:
        # Non-uniform layers: their median height (the add-on does not look the table up per fragment).
        diffs = sorted(t1 - t0 for t0, t1 in zip(tops, tops[1:]) if t1 > t0)
        if diffs:
            height = diffs[len(diffs) // 2]
            first = tops[0]
    if height <= 0:
        return None, 0.0
    tc = b.node("ShaderNodeTexCoord")
    sep = b.node("ShaderNodeSeparateXYZ")
    b.link(tc.outputs["Object"], sep.inputs[0])
    z = b.math("SUBTRACT", sep.outputs["Z"], first)
    phase = b.math("FRACT", b.math("DIVIDE", z, height))
    a = b.math("MULTIPLY_ADD", phase, 2.0, -1.0)
    h = b.math("SQRT", b.math("MAXIMUM", b.math("SUBTRACT", 1.0, b.math("MULTIPLY", a, a)), 0.0))
    return h, height


def build_material(name, spec, layers=None, has_silk_attr=False, imperfection_colour=0.0, layer_bump=True, build_clip=False):
    """The finished material `name` for `spec` (a showcase.json materials entry). Replaces any node
    tree a material of that name had."""
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    compat.enable_nodes(mat)
    tree = mat.node_tree
    tree.nodes.clear()
    b = _Builder(tree)
    out = b.node("ShaderNodeOutputMaterial")
    out.location = (600, 0)
    bsdf = b.node("ShaderNodeBsdfPrincipled")
    bsdf.location = (250, 0)
    b.link(bsdf.outputs[0], out.inputs["Surface"])

    base = spec.get("base_color", [0.8, 0.8, 0.8])
    color = None   # a socket when the colour is procedural
    s = lambda k, d=0.0: float(spec.get(k, d) or 0.0)

    compat.set_input(bsdf, "Base Color", _rgba(base))
    compat.set_input(bsdf, "Roughness", s("roughness", 0.5))
    compat.set_input(bsdf, "Metallic", s("metallic"))
    compat.set_input(bsdf, "IOR", s("ior", 1.5))
    compat.set_input(bsdf, "Specular IOR Level", 0.5)
    tint = s("specular_tint")
    compat.set_input(bsdf, "Specular Tint", tuple(1.0 + (c - 1.0) * tint for c in base) + (1.0,))
    compat.set_input(bsdf, "Anisotropic", s("anisotropic"))
    compat.set_input(bsdf, "Coat Weight", s("coat_weight"))
    compat.set_input(bsdf, "Coat Roughness", s("coat_roughness", 0.05))
    compat.set_input(bsdf, "Sheen Weight", s("sheen_weight"))
    compat.set_input(bsdf, "Sheen Roughness", s("sheen_roughness", 0.5))
    compat.set_input(bsdf, "Transmission Weight", s("transmission_weight"))
    compat.set_input(bsdf, "Subsurface Weight", s("subsurface_weight"))
    if s("subsurface_weight") > 0:
        compat.set_input(bsdf, "Subsurface Scale", s("subsurface_scale_m", 0.001))
        compat.set_input(bsdf, "Subsurface Radius", tuple(spec.get("subsurface_radius", [1, 1, 1])))
    if s("emission_strength") > 0:
        compat.set_input(bsdf, "Emission Color", tuple(spec.get("emission_color", [0, 0, 0])))
        compat.set_input(bsdf, "Emission Strength", s("emission_strength"))

    # Silk: the highlight streaks across the layers, so the tangent runs along them.
    if s("anisotropic") > 0:
        tan_sock = compat.principled_input(bsdf, "Tangent")
        if tan_sock is not None:
            if spec.get("tangent") == "attribute" and has_silk_attr:
                attr = b.node("ShaderNodeAttribute", attribute_name=LINEAR_ATTR)
                b.link(b.vmath("NORMALIZE", attr.outputs["Vector"]), tan_sock)
            else:
                tan = b.node("ShaderNodeTangent", direction_type="RADIAL", axis="Z")
                b.link(tan.outputs["Tangent"], tan_sock)

    # Colour: marble specks, wood bands, a slight variation.
    speckle = spec.get("speckle", "none")
    amount = s("speckle_amount")
    if speckle == "stone" and amount > 0:
        tc = b.node("ShaderNodeTexCoord")
        vor = b.node("ShaderNodeTexVoronoi")
        vor.inputs["Scale"].default_value = 1.0 / 0.0005   # 0.5 mm cells
        b.link(tc.outputs["Object"], vor.inputs["Vector"])
        sep = b.node("ShaderNodeSeparateColor")
        b.link(vor.outputs["Color"], sep.inputs[0])
        dark = b.math("LESS_THAN", sep.outputs[0], 0.18 * amount)
        light = b.math("LESS_THAN", sep.outputs[1], 0.10 * amount)
        color = b.mix_rgb(dark, _rgba(base), tuple(c * 0.2 for c in base))
        color = b.mix_rgb(light, color, (0.9, 0.9, 0.88))
    elif speckle == "wood" and amount > 0:
        tc = b.node("ShaderNodeTexCoord")
        wave = b.node("ShaderNodeTexWave", wave_type="BANDS", bands_direction="Z")
        wave.inputs["Scale"].default_value = 1.0 / 0.004
        wave.inputs["Distortion"].default_value = 6.0
        wave.inputs["Detail"].default_value = 3.0
        b.link(tc.outputs["Object"], wave.inputs["Vector"])
        factor = b.math("MULTIPLY", wave.outputs["Fac"], 0.45 * amount)
        color = b.mix_rgb(factor, _rgba(base), tuple(c * 0.45 for c in base))
    if imperfection_colour > 0:
        tc = b.node("ShaderNodeTexCoord")
        noise = b.node("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = 1.0 / 0.015
        b.link(tc.outputs["Object"], noise.inputs["Vector"])
        hsv = b.node("ShaderNodeHueSaturation")
        if color is not None:
            b.link(color, hsv.inputs["Color"])
        else:
            hsv.inputs["Color"].default_value = _rgba(base)
        b.link(b.math("MULTIPLY_ADD", b.math("SUBTRACT", noise.outputs["Fac"], 0.5), 0.15 * imperfection_colour, 1.0), hsv.inputs["Value"])
        color = hsv.outputs["Color"]
    if color is not None:
        b.link(color, compat.principled_input(bsdf, "Base Color"))

    # Normal: flakes, fibres, layer lines (chained bumps).
    normal = None
    sparkle = s("sparkle")
    if sparkle > 0:
        tc = b.node("ShaderNodeTexCoord")
        geo = b.node("ShaderNodeNewGeometry")
        vor = b.node("ShaderNodeTexVoronoi")
        vor.inputs["Scale"].default_value = 1.0 / max(s("sparkle_cell_m", 0.00015), 1e-5)
        b.link(tc.outputs["Object"], vor.inputs["Vector"])
        sep = b.node("ShaderNodeSeparateColor")
        b.link(vor.outputs["Color"], sep.inputs[0])
        mask = b.math("LESS_THAN", sep.outputs[0], sparkle)
        tilt = b.vmath("MULTIPLY_ADD", vor.outputs["Color"], (2.0, 2.0, 2.0))
        tilt.node.inputs[2].default_value = (-1.0, -1.0, -1.0)
        tilt = b.vmath("SCALE", tilt, scale=b.math("MULTIPLY", mask, 0.7))
        normal = b.vmath("NORMALIZE", b.vmath("ADD", geo.outputs["Normal"], tilt))
        met = b.math("MAXIMUM", mask, s("metallic"))
        b.link(met, compat.principled_input(bsdf, "Metallic"))
    fiber = s("fiber_noise")
    if fiber > 0:
        tc = b.node("ShaderNodeTexCoord")
        noise = b.node("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = 1.0 / 0.00025
        noise.inputs["Detail"].default_value = 2.0
        b.link(tc.outputs["Object"], noise.inputs["Vector"])
        bump = b.node("ShaderNodeBump")
        bump.inputs["Strength"].default_value = min(1.0, 0.35 * fiber)
        bump.inputs["Distance"].default_value = 0.00005
        b.link(noise.outputs["Fac"], bump.inputs["Height"])
        if normal is not None:
            b.link(normal, bump.inputs["Normal"])
        normal = bump.outputs["Normal"]
    bump_strength = s("layer_bump")
    if layer_bump and bump_strength > 0 and layers and layers.get("valid"):
        h, height = _layer_profile(b, layers)
        if h is not None:
            bump = b.node("ShaderNodeBump")
            bump.inputs["Strength"].default_value = min(1.0, 4.0 * bump_strength)
            bump.inputs["Distance"].default_value = 0.5 * height
            b.link(h, bump.inputs["Height"])
            if normal is not None:
                b.link(normal, bump.inputs["Normal"])
            normal = bump.outputs["Normal"]
    if normal is not None:
        b.link(normal, compat.principled_input(bsdf, "Normal"))

    # Coloured clear plastic: Beer-Lambert absorption inside.
    if s("transmission_weight") > 0 and s("absorption_density") > 0:
        vol = b.node("ShaderNodeVolumeAbsorption")
        vol.inputs["Color"].default_value = _rgba(base)
        vol.inputs["Density"].default_value = s("absorption_density")
        b.link(vol.outputs[0], out.inputs["Volume"])

    # A video's build-up: transparent above the scene's keyed height (Showcase's clip fallback).
    if build_clip:
        attr = b.node("ShaderNodeAttribute", attribute_type="VIEW_LAYER", attribute_name=BUILD_PROP)
        geo = b.node("ShaderNodeNewGeometry")
        sep = b.node("ShaderNodeSeparateXYZ")
        b.link(geo.outputs["Position"], sep.inputs[0])
        above = b.math("GREATER_THAN", sep.outputs["Z"], attr.outputs["Fac"])
        clear = b.node("ShaderNodeBsdfTransparent")
        mix = b.node("ShaderNodeMixShader")
        b.link(above, mix.inputs["Fac"])
        b.link(bsdf.outputs[0], mix.inputs[1])
        b.link(clear.outputs[0], mix.inputs[2])
        b.link(mix.outputs[0], out.inputs["Surface"])

    mat["edgeslicer_finish"] = spec.get("finish", "standard")
    mat.diffuse_color = _rgba(base)   # the viewport's solid colour
    if hasattr(mat, "blend_method") and s("transmission_weight") > 0:
        try:
            mat.blend_method = "HASHED"
        except Exception:
            pass
    return mat


def emission_material(name, color, strength):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    compat.enable_nodes(mat)
    tree = mat.node_tree
    tree.nodes.clear()
    out = tree.nodes.new("ShaderNodeOutputMaterial")
    em = tree.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = _rgba(color)
    em.inputs["Strength"].default_value = strength
    tree.links.new(em.outputs[0], out.inputs["Surface"])
    return mat


def deg(v):
    return math.radians(float(v or 0.0))

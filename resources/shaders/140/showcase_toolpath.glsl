// Showcase realism 1b (tests/design_showcase_realism.md 2.2): the toolpaths' lighting.
//
// Not a shader of its own. ShowcaseRenderer puts the SHOWCASE_MATERIAL_COMMON block of
// showcase_material.fs in front of this file (Showcase::toolpath_prelude) and gives the result to
// libvgcode (Viewer::set_fragment_prelude), which compiles it ahead of its smooth-path fragment
// shader with EXTERNAL_LIGHTING defined. libvgcode then calls the two functions below instead of its
// own Preview lighting, so the sliced look and the build-up get the mesh mode's lights, environment
// and finishes. libvgcode keeps the bead normal, the groove and the moire fade; Preview (smooth_mode
// 0) never runs this.
//
// Only names that libvgcode's shader does not declare (see the header of showcase_material.fs).

// Per extruder (libvgcode's look index, min(extruder, 15)): MaterialParams of its finish.
uniform vec4  sc_mat_a[16];          // roughness, f0, metal_tint, anisotropy
uniform vec4  sc_mat_b[16];          // clearcoat, fiber_noise, -, -
uniform float sc_exposure;           // 2^EV, for the in-shader tone map without the HDR layer

// The lit colour of one bead fragment, linear.
//   n         the bead's normal (world), v the unit direction to the eye (world)
//   tangent   the extrusion direction (world), the silk lobe's long axis
//   base      the filament colour, sRGB as libvgcode decodes it
//   p         the fragment's world position
//   groove    libvgcode's groove shade (1 = none)
//   shadow    the key light's shadow (1 = lit)
//   look      the extruder index into sc_mat_a / sc_mat_b
vec3 external_lit(vec3 n, vec3 v, vec3 tangent, vec3 base, vec3 p, float groove, float shadow, int look)
{
    vec4 ma = sc_mat_a[look];
    vec4 mb = sc_mat_b[look];
    vec3 albedo = srgb_to_linear(base);
    float roughness = clamp(ma.x, 0.03, 1.0);
    // Carbon / glass fibre: the mesh's speckle, in world space as there.
    if (mb.y > 0.0) {
        float speck = hash12(floor(p.xy * 12.0) + floor(p.z * 12.0) * 17.0);
        albedo *= 1.0 - mb.y * 0.18 * speck;
        roughness = clamp(roughness - mb.y * 0.25 * step(0.93, speck), 0.03, 1.0);
    }
    // The lobe's tangent: along the extrusion, kept across the normal (a bead's normal is already
    // perpendicular to its direction; this only guards the rounding).
    vec3 t = tangent - n * dot(tangent, n);
    float tl = length(t);
    if (tl < 1e-4) {
        vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        t = normalize(cross(up, n));
    } else
        t /= tl;

    vec3 diffuse, specular;
    shade_material(n, v, t, albedo, roughness, ma.y, ma.z, ma.w, mb.x, shadow, diffuse, specular);
    return diffuse * groove + specular * mix(1.0, groove, 0.5);
}

// What the fragment writes: linear light (alpha 1) into the HDR layer, or tone-mapped sRGB.
vec4 external_output(vec3 lit)
{
    return material_output(lit, 1.0, sc_exposure, true);
}

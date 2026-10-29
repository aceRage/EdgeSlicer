#include "MultiColourGlsl.hpp"

#include <algorithm>
#include <cmath>

#define SLIC3R_MC_STRINGIFY2(...) #__VA_ARGS__
#define SLIC3R_MC_STRINGIFY(...) SLIC3R_MC_STRINGIFY2(__VA_ARGS__)

namespace Slic3r {
namespace MultiColour {

float twist_turns_per_mm3(float twist_deg_per_m, float diameter_mm)
{
    if (!std::isfinite(twist_deg_per_m) || twist_deg_per_m == 0.f)
        return 0.f;
    const double d    = std::isfinite(diameter_mm) && diameter_mm > 0.f ? double(diameter_mm) : 1.75;
    const double area = 3.14159265358979323846 * 0.25 * d * d;   // mm2 of filament
    // turns per metre of filament / (1000 mm per metre * mm2 per mm of filament)
    return float(double(twist_deg_per_m) / 360. / (1000. * area));
}

UniformTable pack_uniforms(const std::vector<Filament> &slots, const std::vector<float> &diameters_mm)
{
    UniformTable t;
    for (int s = 0; s < GLSL_SLOTS; ++s) {
        const Filament f = size_t(s) < slots.size() ? slots[size_t(s)] : Filament();
        const int      n = std::clamp(f.n, 1, MAX_COLOURS);
        for (int c = 0; c < MAX_COLOURS; ++c) {
            const RGB &col = f.colour[size_t(c < n ? c : 0)];
            float     *dst = &t.colors[size_t((s * MAX_COLOURS + c) * 4)];
            dst[0] = col[0];
            dst[1] = col[1];
            dst[2] = col[2];
            dst[3] = 1.f;
        }
        float *b = &t.bounds[size_t(s * 4)];
        b[0] = f.boundary(1);
        b[1] = f.boundary(2);
        b[2] = f.boundary(3);
        b[3] = float(n);
        float narrowest = 1.f;
        for (int i = 0; i < n; ++i)
            narrowest = std::min(narrowest, f.proportion[size_t(i)]);
        float *p = &t.params[size_t(s * 4)];
        p[0] = f.rotation_deg / 360.f;
        p[1] = n > 1 ? twist_turns_per_mm3(f.twist_deg_per_m, size_t(s) < diameters_mm.size() ? diameters_mm[size_t(s)] : 1.75f) : 0.f;
        p[2] = narrowest;
        p[3] = IRONING_TO_AVERAGE;
        t.any_multi |= n > 1;
        t.any_twist |= n > 1 && p[1] != 0.f;
    }
    t.global  = { BLEND_TURNS_WALL, BLEND_TURNS_TOP, STACK_UP_K, WALL_VISIBLE_HEIGHT };
    t.global2 = { TOP_FLATTEN, 0.f, 0.f, 0.f };
    return t;
}

const char *glsl_functions()
{
    return SLIC3R_MC_STRINGIFY(SLIC3R_MC_SHARED_GLSL);
}

const std::string &glsl_prelude()
{
    static const std::string prelude = std::string(
        "uniform vec4 mc_colors[64];\n"
        "uniform vec4 mc_bounds[16];\n"
        "uniform vec4 mc_params[16];\n"
        "uniform vec4 mc_global;\n"
        "uniform vec4 mc_global2;\n") +
        glsl_functions() +
        "\n"
        // libvgcode's hook (Viewer::set_base_color_prelude): section is (+1 right of travel, +1 top),
        // right and dir the segment's right and travel directions, look the slot (role buffer).
        "vec3 external_base_color(vec3 base_color, vec2 section, vec3 right, vec3 dir, float extruded_volume, int look, int role) {\n"
        "  if (look < 0 || look > 15 || mc_count(look) <= 1)\n"
        "    return base_color;\n"
        "  return mc_to_srgb3(mc_fragment_colour(look, role, section.x, section.y, right, dir, extruded_volume));\n"
        "}\n";
    return prelude;
}

} // namespace MultiColour
} // namespace Slic3r

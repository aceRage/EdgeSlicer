#include "MultiColourGlsl.hpp"

#include "../Config.hpp"
#include "../Flow.hpp"
#include "../PrintConfig.hpp"

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

bool uses_split(const UniformTable &table, const std::vector<unsigned char> &slots)
{
    if (!table.any_multi)
        return false;
    for (const unsigned char s : slots)
        if (s < GLSL_SLOTS && table.bounds[size_t(s) * 4 + 3] > 1.5f)
            return true;
    return false;
}

bool uses_twist(const UniformTable &table, const std::vector<unsigned char> &slots)
{
    if (!table.any_twist)
        return false;
    for (const unsigned char s : slots)
        if (s < GLSL_SLOTS && table.bounds[size_t(s) * 4 + 3] > 1.5f && table.params[size_t(s) * 4 + 1] != 0.f)
            return true;
    return false;
}

std::string config_signature(const ConfigBase &config)
{
    std::string sig;
    auto add = [&config, &sig](const std::string &key) {
        if (const ConfigOption *opt = config.option(key); opt != nullptr) {
            sig += key;
            sig += '=';
            sig += opt->serialize();
            sig += ';';
        }
    };
    add("filament_colour");
    add("filament_multi_colors");
    add("filament_colour_mode");
    for (const std::string &key : visual_only_option_keys())
        add(key);
    return sig;
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

const char *glsl_mesh_functions()
{
    return SLIC3R_MC_STRINGIFY(SLIC3R_MC_MESH_GLSL);
}

const std::string &glsl_mesh_prelude()
{
    static const std::string prelude = std::string(
        "#define SHOWCASE_MULTICOLOUR 1\n"
        "uniform vec4 mc_colors[64];\n"
        "uniform vec4 mc_bounds[16];\n"
        "uniform vec4 mc_params[16];\n"
        "uniform vec4 mc_global;\n"
        "uniform vec4 mc_global2;\n") +
        glsl_functions() + "\n" + glsl_mesh_functions() + "\n";
    return prelude;
}

std::string mesh_fragment_source(const std::string &fs)
{
    size_t start = 0;
    // A byte order mark or blank lines may come first; the directive itself must still lead.
    if (fs.compare(0, 3, "\xEF\xBB\xBF") == 0)
        start = 3;
    while (start < fs.size() && (fs[start] == '\r' || fs[start] == '\n' || fs[start] == ' ' || fs[start] == '\t'))
        ++start;
    if (fs.compare(start, 8, "#version") != 0)
        return std::string();
    const size_t eol = fs.find('\n', start);
    if (eol == std::string::npos)
        return std::string();
    return fs.substr(0, eol + 1) + glsl_mesh_prelude() + fs.substr(eol + 1);
}

MeshSkin mesh_skin(const ConfigBase &config, int extruder)
{
    MeshSkin out;
    double nozzle = 0.4;
    if (const auto *n = config.option<ConfigOptionFloats>("nozzle_diameter"); n != nullptr && !n->values.empty()) {
        const size_t i = size_t(std::clamp(extruder - 1, 0, int(n->values.size()) - 1));
        if (std::isfinite(n->values[i]) && n->values[i] > 0.)
            nozzle = n->values[i];
    }
    auto abs_value = [&config, nozzle](const char *key) {
        const auto *o = config.option<ConfigOptionFloatOrPercent>(key);
        const double v = o != nullptr ? o->get_abs_value(nozzle) : 0.;
        return std::isfinite(v) && v > 0. ? v : 0.;
    };
    auto height = [&config](const char *key, double fallback) {
        const auto *o = config.option<ConfigOptionFloat>(key);
        return o != nullptr && std::isfinite(o->value) && o->value > 0. ? o->value : fallback;
    };
    auto spacing = [&](const char *key, FlowRole role, double layer_height) {
        double w = abs_value(key);
        if (w <= 0.)
            w = abs_value("line_width");
        if (w <= 0.)
            w = Flow::auto_extrusion_width(role, float(nozzle));
        // Flow::spacing(): a rounded bead overlaps its neighbour by h (1 - pi / 4).
        const double s = w - layer_height * (1. - 0.25 * 3.14159265358979323846);
        return float(s > 0.05 ? s : w);
    };
    const double layer_height = height("layer_height", 0.2);
    out.top_spacing    = spacing("top_surface_line_width", frTopSolidInfill, layer_height);
    out.bottom_spacing = spacing("internal_solid_infill_line_width", frSolidInfill, layer_height);
    out.first_spacing  = spacing("initial_layer_line_width", frPerimeter, height("initial_layer_print_height", layer_height));
    if (const ConfigOption *o = config.option("wall_direction"); o != nullptr && o->serialize() == "cw")
        out.wall_dir = -1.f;
    out.layer_height = float(layer_height);
    // The band of walls a slope's layer has to step past before its skin shows.
    int loops = 2;
    if (const auto *o = config.option<ConfigOptionInt>("wall_loops"); o != nullptr)
        loops = std::max(1, o->value);
    out.wall_band = spacing("outer_wall_line_width", frExternalPerimeter, layer_height) +
                    float(loops - 1) * spacing("inner_wall_line_width", frPerimeter, layer_height);
    return out;
}

} // namespace MultiColour
} // namespace Slic3r

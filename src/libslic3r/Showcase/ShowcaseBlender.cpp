#include "ShowcaseBlender.hpp"

#include "ShowcaseAssets.hpp"
#include "ShowcaseLens.hpp"
#include "ShowcasePath.hpp"
#include "ShowcaseTone.hpp"

#include "../miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

namespace Slic3r {
namespace Showcase {

namespace {

using bp_json = nlohmann::json;
constexpr double BP_PI = 3.14159265358979323846;

// The shortest decimal that reads back as the same float (see gltf_num): readable, deterministic JSON.
double bp_num(double v)
{
    if (!std::isfinite(v))
        return 0.;
    for (int p = 1; p <= 9; ++p) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*g", p, float(v));
        if (float(std::strtod(buf, nullptr)) == float(v))
            return std::strtod(buf, nullptr);
    }
    return double(float(v));
}

template<size_t N, class T> bp_json bp_vec(const std::array<T, N> &a)
{
    bp_json j = bp_json::array();
    for (const T &v : a)
        j.push_back(bp_num(double(v)));
    return j;
}

std::vector<uint8_t> bp_bytes(const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::array<double, 3> bp_sub(const std::array<double, 3> &a, const std::array<double, 3> &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
double bp_dot(const std::array<double, 3> &a, const std::array<double, 3> &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double bp_len(const std::array<double, 3> &a) { return std::sqrt(bp_dot(a, a)); }
std::array<double, 3> bp_unit(const std::array<double, 3> &a, const std::array<double, 3> &fallback)
{
    const double l = bp_len(a);
    return l > 1e-12 ? std::array<double, 3>{ a[0] / l, a[1] / l, a[2] / l } : fallback;
}

std::string bp_clean_name(const std::string &s)
{
    std::string out;
    for (char c : s)
        out.push_back((unsigned char)c < 32 ? ' ' : c);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? std::string("Part") : out;
}

bp_json bp_layers(const PackageLayers &l)
{
    bp_json j;
    j["valid"] = l.valid;
    if (!l.valid)
        return j;
    j["uniform"]    = l.uniform;
    j["first_m"]    = bp_num(l.first * BLENDER_MM_TO_M);
    j["height_m"]   = bp_num(l.height * BLENDER_MM_TO_M);
    if (!l.uniform) {
        bp_json tops = bp_json::array();
        const size_t n = std::min<size_t>(l.tops.size(), 4096);
        for (size_t i = 0; i < n; ++i)
            tops.push_back(bp_num(l.tops[i] * BLENDER_MM_TO_M));
        j["tops_m"] = tops;
    }
    return j;
}

bp_json bp_camera(const BlenderCamera &c)
{
    bp_json j;
    j["type"]             = c.ortho ? "ortho" : "persp";
    j["location"]         = bp_vec(c.location);
    j["target"]           = bp_vec(c.target);
    j["up"]               = bp_vec(c.up);
    j["fov_y_deg"]        = bp_num(c.fov_y_deg);
    j["ortho_height"]     = bp_num(c.ortho_height);
    j["sensor_fit"]       = "VERTICAL";
    j["sensor_height_mm"] = bp_num(c.sensor_height_mm);
    j["lens_mm"]          = bp_num(c.lens_mm);
    j["clip_start"]       = bp_num(c.clip_start);
    j["clip_end"]         = bp_num(c.clip_end);
    return j;
}

std::string bp_image_ext(const std::string &ext)
{
    std::string e = ext;
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == "jpeg" ? "jpg" : e.empty() ? "png" : e;
}

bool bp_fail(std::string *error, const std::string &what)
{
    if (error != nullptr)
        *error = what;
    return false;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Units and colours
// ------------------------------------------------------------------------------------------------

std::array<float, 3> gltf_from_mm(double x, double y, double z)
{
    return { float(x * BLENDER_MM_TO_M), float(z * BLENDER_MM_TO_M), float(-y * BLENDER_MM_TO_M) };
}

std::array<double, 3> blender_from_mm(const std::array<double, 3> &mm)
{
    return { mm[0] * BLENDER_MM_TO_M, mm[1] * BLENDER_MM_TO_M, mm[2] * BLENDER_MM_TO_M };
}

float srgb_to_linear(float c)
{
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::array<float, 3> hex_to_linear(const std::string &hex, const std::array<float, 3> &fallback)
{
    std::array<float, 4> rgba;
    if (!parse_hex_color(hex, rgba))
        return fallback;
    return { srgb_to_linear(rgba[0]), srgb_to_linear(rgba[1]), srgb_to_linear(rgba[2]) };
}

// ------------------------------------------------------------------------------------------------
// Materials
// ------------------------------------------------------------------------------------------------

BlenderMaterial blender_material(Finish finish, const std::string &hex_color, float translucency, LayerLines layer_lines, bool tubes,
                                 const BlenderGlow &glow, bool lights_off)
{
    const MaterialParams p  = material_params(finish);
    const Translucency   tr = effective_translucency(finish, translucency);
    BlenderMaterial      m;
    m.finish        = to_string(finish);
    m.base_color    = hex_to_linear(hex_color);
    m.roughness     = std::clamp(p.roughness, 0.f, 1.f);
    // F0 = ((n - 1) / (n + 1))^2  ->  n = (1 + sqrt F0) / (1 - sqrt F0); a metal-filled plastic keeps a
    // dielectric IOR and gets its colour from the metallic input instead.
    const float sf0 = std::sqrt(std::clamp(finish == Finish::Metallic ? 0.05f : p.f0, 0.f, 0.25f));
    m.ior           = std::clamp((1.f + sf0) / (1.f - sf0), 1.2f, 3.f);
    m.metallic      = finish == Finish::Metallic ? 0.7f * p.metal_tint : 0.f;
    m.specular_tint = finish == Finish::Metallic ? 0.f : std::clamp(p.metal_tint, 0.f, 1.f);
    m.anisotropic   = std::clamp(p.anisotropy, 0.f, 1.f);
    m.tangent       = tubes ? "attribute" : "radial_z";
    m.coat_weight   = std::clamp(p.clearcoat, 0.f, 1.f);
    m.sheen_weight  = std::clamp(std::max(p.sheen, 0.4f * p.fiber_noise), 0.f, 1.f);
    m.fiber_noise   = p.fiber_noise;
    m.sparkle       = p.sparkle;
    m.sparkle_cell_m = p.sparkle > 0.f ? float(SPARKLE_CELL_MM * BLENDER_MM_TO_M) : 0.f;
    m.speckle        = p.speckle_kind == SpeckleKind::Stone ? "stone" : p.speckle_kind == SpeckleKind::Wood ? "wood" : "none";
    m.speckle_amount = p.speckle_kind == SpeckleKind::None ? 0.f : p.speckle;
    m.layer_bump     = tubes ? 0.f : layer_line_depth(layer_lines) * p.line_shade;

    // Light through the plastic (1d). Clear plastic transmits; milky plastic scatters inside (subsurface)
    // and lets a little through; the rest only soften at the edges.
    if (finish == Finish::Transparent) {
        m.transmission_weight = std::max(0.9f, tr.transmission);
        m.ior                 = 1.57f;   // PETG / PLA
    } else if (finish == Finish::Translucent) {
        m.transmission_weight = 0.3f * tr.transmission;
        m.ior                 = 1.57f;
    } else
        m.transmission_weight = 0.5f * tr.transmission;
    const float density = std::max(tr.density, finish == Finish::Transparent ? 0.12f : 0.f);
    if (m.transmission_weight > 0.f) {
        m.thickness_m        = float(DEFAULT_WALL_THICKNESS * BLENDER_MM_TO_M);
        m.absorption_density = float(density / BLENDER_MM_TO_M);   // per mm -> per m
    }
    if (tr.sss > 0.f) {
        m.subsurface_weight  = std::clamp(tr.sss, 0.f, 1.f);
        // How far light wanders: the mean free path 1 / density (mm), halved (Principled's radius is a
        // scattering distance, not a full path).
        m.subsurface_scale_m = float(0.5 / std::max(0.2, double(density > 0.f ? density : 0.5f)) * BLENDER_MM_TO_M);
        for (int c = 0; c < 3; ++c)
            m.subsurface_radius[size_t(c)] = 0.5f + 0.5f * std::sqrt(m.base_color[size_t(c)]);
    }
    if (glow.on && lights_off) {
        m.emission_color    = { srgb_to_linear(glow.srgb[0]), srgb_to_linear(glow.srgb[1]), srgb_to_linear(glow.srgb[2]) };
        m.emission_strength = 2.f * std::clamp(glow.intensity, 0.f, 3.f);
    }
    return m;
}

GltfMaterial gltf_material(const BlenderMaterial &m, const std::string &name, const std::string &extras_json)
{
    GltfMaterial g;
    g.name       = name;
    g.base_color = { m.base_color[0], m.base_color[1], m.base_color[2], 1.f };
    g.metallic   = m.metallic;
    g.roughness  = m.roughness;
    const float emax = std::max({ m.emission_color[0], m.emission_color[1], m.emission_color[2] });
    if (emax > 0.f && m.emission_strength > 0.f) {
        g.emissive          = { m.emission_color[0] / emax, m.emission_color[1] / emax, m.emission_color[2] / emax };
        g.emissive_strength = std::max(1.f, emax * m.emission_strength);
    }
    g.clearcoat           = m.coat_weight;
    g.clearcoat_roughness = m.coat_roughness;
    g.transmission        = m.transmission_weight;
    g.ior                 = m.ior;
    if (m.transmission_weight > 0.f) {
        g.thickness         = m.thickness_m;
        g.attenuation_color = m.base_color;
        if (m.absorption_density > 0.f)
            g.attenuation_distance = 1.f / m.absorption_density;
    }
    if (m.sheen_weight > 0.f) {
        g.sheen_color     = { m.sheen_weight, m.sheen_weight, m.sheen_weight };
        g.sheen_roughness = m.sheen_roughness;
    }
    for (int c = 0; c < 3; ++c)
        g.specular_color[size_t(c)] = 1.f + (m.base_color[size_t(c)] - 1.f) * m.specular_tint;
    g.anisotropy   = m.anisotropic;
    g.double_sided = m.transmission_weight > 0.f;
    g.extras_json  = extras_json;
    return g;
}

static bp_json blender_material_object(const BlenderMaterial &m)
{
    bp_json j;
    j["finish"]              = m.finish;
    j["base_color"]          = bp_vec(m.base_color);
    j["roughness"]           = bp_num(m.roughness);
    j["metallic"]            = bp_num(m.metallic);
    j["ior"]                 = bp_num(m.ior);
    j["specular_tint"]       = bp_num(m.specular_tint);
    j["anisotropic"]         = bp_num(m.anisotropic);
    j["tangent"]             = m.tangent;
    j["coat_weight"]         = bp_num(m.coat_weight);
    j["coat_roughness"]      = bp_num(m.coat_roughness);
    j["sheen_weight"]        = bp_num(m.sheen_weight);
    j["sheen_roughness"]     = bp_num(m.sheen_roughness);
    j["transmission_weight"] = bp_num(m.transmission_weight);
    j["thickness_m"]         = bp_num(m.thickness_m);
    j["absorption_density"]  = bp_num(m.absorption_density);
    j["subsurface_weight"]   = bp_num(m.subsurface_weight);
    j["subsurface_scale_m"]  = bp_num(m.subsurface_scale_m);
    j["subsurface_radius"]   = bp_vec(m.subsurface_radius);
    j["emission_color"]      = bp_vec(m.emission_color);
    j["emission_strength"]   = bp_num(m.emission_strength);
    j["sparkle"]             = bp_num(m.sparkle);
    j["sparkle_cell_m"]      = bp_num(m.sparkle_cell_m);
    j["fiber_noise"]         = bp_num(m.fiber_noise);
    j["speckle"]             = m.speckle;
    j["speckle_amount"]      = bp_num(m.speckle_amount);
    j["layer_bump"]          = bp_num(m.layer_bump);
    return j;
}

std::string blender_material_json(const BlenderMaterial &m) { return blender_material_object(m).dump(); }

// ------------------------------------------------------------------------------------------------
// Camera and lights
// ------------------------------------------------------------------------------------------------

BlenderCamera blender_camera(const CameraPose &pose, Projection projection, double frame_h_px, double aspect)
{
    BlenderCamera c;
    c.ortho    = projection == Projection::Ortho;
    c.location = blender_from_mm(pose.eye);
    c.target   = blender_from_mm(pose.target);
    c.up       = bp_unit(pose.up, { 0., 0., 1. });
    aspect     = aspect > 1e-6 && std::isfinite(aspect) ? aspect : 16. / 9.;
    const double view_h   = path_view_height(pose.zoom, std::max(1., frame_h_px));   // mm at the target
    const double distance = bp_len(bp_sub(pose.eye, pose.target));
    c.fov_y_deg        = std::clamp(path_fov_deg(view_h, distance), 0.5, 170.);
    c.ortho_height     = view_h * BLENDER_MM_TO_M;
    c.sensor_height_mm = 2. * half_sensor_height_mm(aspect);
    c.lens_mm          = focal_from_fov_y(c.fov_y_deg * BP_PI / 180., aspect);
    c.clip_start       = 0.0005;
    c.clip_end         = 2000.;
    return c;
}

std::vector<BlenderLight> blender_lights(const Lighting &lighting, const std::array<double, 3> &view_dir, bool key_from_hdri, float dim)
{
    std::vector<BlenderLight> out;
    const std::vector<LightSource> src = three_point_lights(lighting);
    // three_point_lights() gives the key first, then the fill and the rim when on.
    std::vector<std::string> order = { "Key" };
    if (lighting.fill && lighting.fill_intensity > 0.f)
        order.push_back("Fill");
    if (lighting.rim && lighting.rim_intensity > 0.f)
        order.push_back("Rim");
    if (!(lighting.key_intensity > 0.f))
        order.erase(order.begin());
    const float keep  = std::clamp(1.f - dim, 0.f, 1.f);
    const float angle = std::clamp(3.f * lighting.shadow_softness, 0.5f, 20.f);
    for (size_t i = 0; i < src.size() && i < order.size(); ++i) {
        if (order[i] == "Key" && key_from_hdri)
            continue;
        BlenderLight l;
        l.name = "EdgeSlicer " + order[i];
        l.direction = { double(src[i].dir[0]), double(src[i].dir[1]), double(src[i].dir[2]) };
        if (order[i] == "Key" && !lighting.world_fixed)
            l.direction = bp_unit(view_dir, l.direction);
        const float cmax = std::max({ src[i].color[0], src[i].color[1], src[i].color[2] });
        if (!(cmax > 0.f))
            continue;
        l.color     = { src[i].color[0] / cmax, src[i].color[1] / cmax, src[i].color[2] / cmax };
        l.strength  = float(BP_PI) * cmax * keep;
        l.angle_deg = angle;
        out.push_back(l);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Normals
// ------------------------------------------------------------------------------------------------

NormalMesh mesh_with_normals(const std::vector<float> &vertices, const std::vector<uint32_t> &indices, float angle_deg)
{
    NormalMesh out;
    const size_t nv = vertices.size() / 3;
    const size_t nf = indices.size() / 3;
    std::vector<std::array<double, 3>> fn(nf);
    std::vector<double>                farea(nf);
    std::vector<std::vector<uint32_t>> vfaces(nv);
    for (size_t f = 0; f < nf; ++f) {
        const uint32_t i0 = indices[f * 3], i1 = indices[f * 3 + 1], i2 = indices[f * 3 + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv)
            continue;
        const std::array<double, 3> a{ vertices[i0 * 3], vertices[i0 * 3 + 1], vertices[i0 * 3 + 2] };
        const std::array<double, 3> b{ vertices[i1 * 3], vertices[i1 * 3 + 1], vertices[i1 * 3 + 2] };
        const std::array<double, 3> c{ vertices[i2 * 3], vertices[i2 * 3 + 1], vertices[i2 * 3 + 2] };
        const std::array<double, 3> e1 = bp_sub(b, a), e2 = bp_sub(c, a);
        const std::array<double, 3> n{ e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        farea[f] = 0.5 * bp_len(n);
        fn[f]    = bp_unit(n, { 0., 0., 1. });
        vfaces[i0].push_back(uint32_t(f));
        vfaces[i1].push_back(uint32_t(f));
        vfaces[i2].push_back(uint32_t(f));
    }
    const double cos_limit = std::cos(double(std::clamp(angle_deg, 0.f, 180.f)) * BP_PI / 180.);
    // A corner's normal: the area-weighted normals of the faces at its vertex that are within the angle
    // of its own face. Corners with the same vertex and the same normal share one output vertex.
    std::vector<std::map<std::array<int64_t, 3>, uint32_t>> made(nv);
    out.indices.reserve(indices.size());
    for (size_t f = 0; f < nf; ++f) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = indices[f * 3 + size_t(k)];
            if (v >= nv) {
                out.indices.clear();
                break;
            }
            std::array<double, 3> sum{ 0., 0., 0. };
            for (uint32_t g : vfaces[v])
                if (bp_dot(fn[g], fn[f]) >= cos_limit)
                    for (int c = 0; c < 3; ++c)
                        sum[size_t(c)] += fn[g][size_t(c)] * farea[g];
            const std::array<double, 3> n = bp_unit(sum, fn[f]);
            const std::array<int64_t, 3> key{ int64_t(std::llround(n[0] * 1e5)), int64_t(std::llround(n[1] * 1e5)), int64_t(std::llround(n[2] * 1e5)) };
            auto it = made[v].find(key);
            if (it == made[v].end()) {
                const uint32_t id = uint32_t(out.positions.size() / 3);
                out.positions.insert(out.positions.end(), { vertices[v * 3], vertices[v * 3 + 1], vertices[v * 3 + 2] });
                out.normals.insert(out.normals.end(), { float(n[0]), float(n[1]), float(n[2]) });
                it = made[v].emplace(key, id).first;
            }
            out.indices.push_back(it->second);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// A bundled environment atlas as a panorama
// ------------------------------------------------------------------------------------------------

bool atlas_to_equirect(const std::vector<uint8_t> &rgba, int atlas_w, int atlas_h, int face_size, int levels, float rgbm_range, int width,
                       HdrImage &out)
{
    out = HdrImage();
    if (face_size <= 0 || levels <= 0 || width < 8 || atlas_w != env_atlas_width(face_size) || atlas_h < env_atlas_height(face_size, levels) ||
        rgba.size() < size_t(atlas_w) * size_t(atlas_h) * 4)
        return false;
    out.width  = width;
    out.height = width / 2;
    out.rgb.assign(size_t(out.width) * size_t(out.height) * 3, 0.f);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const std::array<float, 3> w = equirect_direction((float(x) + 0.5f) / float(out.width), (float(y) + 0.5f) / float(out.height));
            const std::array<float, 3> c = world_to_cube(w);
            const float ax = std::abs(c[0]), ay = std::abs(c[1]), az = std::abs(c[2]);
            int   face;
            float sc, tc, ma;
            // The GL cube map face selection (OpenGL 4.6, table 8.19).
            if (ax >= ay && ax >= az) {
                face = c[0] > 0 ? 0 : 1; ma = ax; sc = c[0] > 0 ? -c[2] : c[2]; tc = -c[1];
            } else if (ay >= az) {
                face = c[1] > 0 ? 2 : 3; ma = ay; sc = c[0]; tc = c[1] > 0 ? c[2] : -c[2];
            } else {
                face = c[2] > 0 ? 4 : 5; ma = az; sc = c[2] > 0 ? c[0] : -c[0]; tc = -c[1];
            }
            const float s = 0.5f * (sc / ma + 1.f), t = 0.5f * (tc / ma + 1.f);
            const RectI r  = env_atlas_face(face_size, levels, 0, face);
            const int   px = std::clamp(r.x + int(s * float(r.w)), r.x, r.x + r.w - 1);
            const int   py = std::clamp(r.y + int(t * float(r.h)), r.y, r.y + r.h - 1);
            const uint8_t *p = &rgba[(size_t(py) * size_t(atlas_w) + size_t(px)) * 4];
            const std::array<float, 3> lin = decode_rgbm(p[0], p[1], p[2], p[3], rgbm_range);
            float *o = &out.rgb[(size_t(y) * size_t(out.width) + size_t(x)) * 3];
            o[0] = lin[0];
            o[1] = lin[1];
            o[2] = lin[2];
        }
    return true;
}

// ------------------------------------------------------------------------------------------------
// The package
// ------------------------------------------------------------------------------------------------

std::string package_readme(const std::string &edgeslicer_version)
{
    return "EdgeSlicer Blender package (edgeslicer-blender-package, version 1)\r\n"
           "Written by EdgeSlicer " + edgeslicer_version + ".\r\n"
           "\r\n"
           "This folder (or zip) describes one Showcase scene for Blender:\r\n"
           "\r\n"
           "  scene.glb          the plate's parts or toolpaths with their materials (glTF 2.0, metres)\r\n"
           "  showcase.json      camera, lights, environment, background, floor and look\r\n"
           "  camera_path.json   the video's camera, one entry per frame (videos only)\r\n"
           "  environment/       the HDRI the scene is lit by\r\n"
           "  textures/          the backplate and floor pictures\r\n"
           "  overlays/          callouts and watermark as a transparent PNG (stills)\r\n"
           "\r\n"
           "With the EdgeSlicer Render add-on (shipped with EdgeSlicer in resources/blender, or installed\r\n"
           "from EdgeSlicer's Showcase tab): File > Import > EdgeSlicer Showcase package, or let EdgeSlicer's\r\n"
           "\"Open in Blender\" do it. The add-on builds a Cycles scene that matches the Showcase framing,\r\n"
           "lighting and finishes, and adds an EdgeSlicer panel to the 3D view's sidebar (N).\r\n"
           "\r\n"
           "Without the add-on: File > Import > glTF 2.0 and pick scene.glb. You get the models with\r\n"
           "plain PBR materials; the camera, lights and environment are only in showcase.json.\r\n"
           "\r\n"
           "The HDRIs come from Poly Haven (CC0). Your own pictures stay yours.\r\n";
}

bool build_blender_package(const PackageInput &in, PackageResult &out, std::string *error)
{
    out = PackageResult();
    const Scene &s = in.scene;
    if (in.width <= 0 || in.height <= 0)
        return bp_fail(error, "the output size is empty");
    if (!in.toolpaths && in.parts.empty())
        return bp_fail(error, "nothing to export: the plate has no parts");
    if (in.toolpaths && in.tubes.meshes.empty())
        return bp_fail(error, "nothing to export: the plate's toolpaths gave no extrusions");

    const double aspect  = double(in.width) / double(in.height);
    const double frame_h = in.frame_height_px > 0. ? in.frame_height_px : double(in.height);
    const float  dim     = s.materials.scene_dim();
    const std::array<double, 3> centre_mm{ 0.5 * (in.model_min[0] + in.model_max[0]), 0.5 * (in.model_min[1] + in.model_max[1]),
                                           0.5 * (in.model_min[2] + in.model_max[2]) };
    const double diag_mm = bp_len(bp_sub(in.model_max, in.model_min));

    auto filament_of = [&in](int slot) -> PackageFilament {
        for (const PackageFilament &f : in.filaments)
            if (f.slot == slot)
                return f;
        PackageFilament f;
        f.slot = slot;
        return f;
    };

    // --- the GLB ----------------------------------------------------------------------------------
    GltfDocument doc;
    doc.generator = "EdgeSlicer " + in.edgeslicer_version;
    std::map<std::pair<int, bool>, int> material_index;   // (slot, tubes) -> index
    bp_json materials_json = bp_json::object();
    auto material_for = [&](int slot, bool tubes) -> int {
        const auto key = std::make_pair(slot, tubes);
        auto it = material_index.find(key);
        if (it != material_index.end())
            return it->second;
        const PackageFilament f = filament_of(slot);
        const BlenderMaterial bm = blender_material(f.finish, f.color, s.materials.translucency, s.materials.layer_lines, tubes, f.glow,
                                                    s.materials.lights_off);
        const std::string name = "EdgeSlicer F" + std::to_string(slot) + " " + bm.finish + (tubes ? " beads" : "");
        bp_json extras;
        extras["edgeslicer"]["filament"] = slot;
        extras["edgeslicer"]["finish"]   = bm.finish;
        extras["edgeslicer"]["color"]    = f.color;
        extras["edgeslicer"]["blender"]  = blender_material_object(bm);
        doc.materials.push_back(gltf_material(bm, name, extras.dump()));
        bp_json mj = blender_material_object(bm);
        mj["filament"]    = slot;
        mj["color"]       = f.color;
        mj["preset"]      = f.preset_name;
        mj["type"]        = f.type;
        mj["glow"]        = f.glow.on;
        materials_json[name] = mj;
        const int idx = int(doc.materials.size()) - 1;
        material_index.emplace(key, idx);
        return idx;
    };

    std::set<std::string> used_names;
    auto unique_name = [&used_names](std::string name) {
        std::string candidate = name;
        for (int k = 2; !used_names.insert(candidate).second; ++k)
            candidate = name + " #" + std::to_string(k);
        return candidate;
    };
    bp_json objects_json = bp_json::array();

    if (!in.toolpaths) {
        for (const PackagePart &part : in.parts) {
            if (part.indices.size() < 3 || part.vertices.size() < 9)
                continue;
            GltfMesh mesh;
            const std::string name = unique_name(bp_clean_name(part.name));
            mesh.name = name;
            if (in.smooth_normals) {
                NormalMesh nm = mesh_with_normals(part.vertices, part.indices, BLENDER_SMOOTH_ANGLE_DEG);
                mesh.indices = std::move(nm.indices);
                mesh.positions.reserve(nm.positions.size());
                mesh.normals.reserve(nm.normals.size());
                for (size_t i = 0; i + 2 < nm.positions.size(); i += 3) {
                    const std::array<float, 3> p = gltf_from_mm(nm.positions[i], nm.positions[i + 1], nm.positions[i + 2]);
                    mesh.positions.insert(mesh.positions.end(), p.begin(), p.end());
                    // Directions turn like positions, without the scale.
                    mesh.normals.insert(mesh.normals.end(), { nm.normals[i], nm.normals[i + 2], -nm.normals[i + 1] });
                }
            } else {
                for (size_t i = 0; i + 2 < part.vertices.size(); i += 3) {
                    const std::array<float, 3> p = gltf_from_mm(part.vertices[i], part.vertices[i + 1], part.vertices[i + 2]);
                    mesh.positions.insert(mesh.positions.end(), p.begin(), p.end());
                }
                mesh.indices = part.indices;
            }
            mesh.material = material_for(part.filament, false);
            out.triangles += mesh.indices.size() / 3;
            doc.meshes.push_back(std::move(mesh));
            GltfNode node;
            node.name = name;
            node.mesh = int(doc.meshes.size()) - 1;
            bp_json extras;
            extras["edgeslicer"]["kind"]     = "part";
            extras["edgeslicer"]["filament"] = part.filament;
            extras["edgeslicer"]["object"]   = part.object_idx;
            extras["edgeslicer"]["volume"]   = part.volume_idx;
            extras["edgeslicer"]["instance"] = part.instance_idx;
            extras["edgeslicer"]["layers"]   = bp_layers(part.layers);
            node.extras_json = extras.dump();
            doc.nodes.push_back(node);
            bp_json oj;
            oj["node"]     = name;
            oj["kind"]     = "part";
            oj["filament"] = part.filament;
            oj["material"] = doc.materials[size_t(doc.meshes.back().material)].name;
            oj["layers"]   = bp_layers(part.layers);
            objects_json.push_back(oj);
        }
    } else {
        for (const TubeMesh &t : in.tubes.meshes) {
            if (t.indices.empty())
                continue;
            const int slot = t.extruder + 1;
            GltfMesh mesh;
            const std::string name = unique_name("Toolpaths F" + std::to_string(slot));
            mesh.name = name;
            mesh.positions.reserve(t.positions.size());
            mesh.normals.reserve(t.normals.size());
            GltfAttribute dir{ "_SILK_DIR", 3, {} };
            GltfAttribute build{ "_BUILD", 1, t.build };
            dir.data.reserve(t.direction.size());
            for (size_t i = 0; i + 2 < t.positions.size(); i += 3) {
                const std::array<float, 3> p = gltf_from_mm(t.positions[i], t.positions[i + 1], t.positions[i + 2]);
                mesh.positions.insert(mesh.positions.end(), p.begin(), p.end());
                mesh.normals.insert(mesh.normals.end(), { t.normals[i], t.normals[i + 2], -t.normals[i + 1] });
                dir.data.insert(dir.data.end(), { t.direction[i], t.direction[i + 2], -t.direction[i + 1] });
            }
            mesh.attributes.push_back(std::move(dir));
            mesh.attributes.push_back(std::move(build));
            mesh.indices  = t.indices;
            mesh.material = material_for(slot, true);
            out.triangles += mesh.indices.size() / 3;
            doc.meshes.push_back(std::move(mesh));
            GltfNode node;
            node.name = name;
            node.mesh = int(doc.meshes.size()) - 1;
            bp_json extras;
            extras["edgeslicer"]["kind"]     = "toolpaths";
            extras["edgeslicer"]["filament"] = slot;
            node.extras_json = extras.dump();
            doc.nodes.push_back(node);
            bp_json oj;
            oj["node"]     = name;
            oj["kind"]     = "toolpaths";
            oj["filament"] = slot;
            oj["material"] = doc.materials[size_t(doc.meshes.back().material)].name;
            objects_json.push_back(oj);
        }
    }
    if (doc.nodes.empty())
        return bp_fail(error, "nothing to export: no part has any triangles");
    {
        bp_json se;
        se["edgeslicer"]["format"]  = BLENDER_PACKAGE_FORMAT;
        se["edgeslicer"]["version"] = BLENDER_PACKAGE_VERSION;
        doc.scene_extras_json = se.dump();
    }
    out.nodes = doc.nodes.size();
    out.files.push_back({ "scene.glb", write_glb(doc) });

    // --- environment, pictures, overlay -----------------------------------------------------------
    std::string hdri_path;
    if (in.has_hdri && !in.hdri.bytes.empty()) {
        std::string stem;
        for (char c : in.hdri.id)
            stem.push_back(std::isalnum((unsigned char)c) || c == '_' || c == '-' ? c : '_');
        if (stem.empty())
            stem = "environment";
        hdri_path = "environment/" + stem + "." + (in.hdri.ext == "exr" ? "exr" : "hdr");
        out.files.push_back({ hdri_path, in.hdri.bytes });
    }
    std::string background_path, floor_path, overlay_path;
    if (!in.background_image.empty()) {
        background_path = "textures/background." + bp_image_ext(in.background_image.ext);
        out.files.push_back({ background_path, in.background_image.bytes });
    }
    if (!in.floor_image.empty()) {
        floor_path = "textures/floor." + bp_image_ext(in.floor_image.ext);
        out.files.push_back({ floor_path, in.floor_image.bytes });
    }
    if (!in.overlay_png.empty() && in.frames.empty()) {
        overlay_path = "overlays/overlay.png";
        out.files.push_back({ overlay_path, in.overlay_png });
    }

    // --- showcase.json ----------------------------------------------------------------------------
    bp_json j;
    j["format"]    = BLENDER_PACKAGE_FORMAT;
    j["version"]   = BLENDER_PACKAGE_VERSION;
    j["generator"] = "EdgeSlicer " + in.edgeslicer_version;
    j["units"]["length"]         = "m";
    j["units"]["up"]             = "+Z";
    j["units"]["metres_per_mm"]  = BLENDER_MM_TO_M;
    j["glb"]                     = "scene.glb";
    j["source"]                  = in.toolpaths ? "toolpaths" : "model";
    j["scene_name"]              = s.name;
    j["objects"]                 = objects_json;
    j["materials"]               = materials_json;

    j["output"]["width"]       = in.width;
    j["output"]["height"]      = in.height;
    j["output"]["fps"]         = in.fps;
    j["output"]["frames"]      = in.frames.size();
    j["output"]["transparent"] = s.background.type == BackgroundType::Transparent;

    j["look"]["preset"]   = s.preset_base;
    j["look"]["tone"]     = to_string(s.lighting.tone);
    j["look"]["exposure"] = bp_num(s.lighting.exposure);
    j["look"]["quality"]  = to_string(s.export_settings.quality);

    // Camera (the still's; a video's moves are in camera_path.json).
    CameraPose still;
    still.eye    = s.camera.eye;
    still.target = s.camera.target;
    still.up     = s.camera.up;
    still.zoom   = s.camera.zoom;
    const BlenderCamera cam = blender_camera(still, s.camera.projection, frame_h, aspect);
    bp_json cj = bp_camera(cam);
    {
        const std::array<double, 3> fwd = bp_unit(bp_sub(cam.target, cam.location), { 0., 1., 0. });
        const std::array<double, 3> focus_point = s.dof.focus_on_model ? blender_from_mm(centre_mm) : std::array<double, 3>{
            cam.location[0] + fwd[0] * s.dof.focus_mm * BLENDER_MM_TO_M, cam.location[1] + fwd[1] * s.dof.focus_mm * BLENDER_MM_TO_M,
            cam.location[2] + fwd[2] * s.dof.focus_mm * BLENDER_MM_TO_M };
        cj["dof"]["enabled"]        = s.dof.enabled && !cam.ortho;
        cj["dof"]["f_number"]       = bp_num(s.dof.f_number);
        cj["dof"]["focus_on_model"] = s.dof.focus_on_model;
        cj["dof"]["focus_point"]    = bp_vec(focus_point);
        cj["dof"]["focus_distance"] = bp_num(std::max(1e-4, bp_dot(bp_sub(focus_point, cam.location), fwd)));
    }
    j["camera"] = cj;

    // Lights.
    const bool key_from_hdri = s.lighting.sun_from_env && in.has_hdri && in.hdri.sun_found;
    const std::array<double, 3> view_dir = bp_unit(bp_sub(s.camera.eye, s.camera.target), { 0., -1., 1. });
    bp_json lights = bp_json::array();
    for (const BlenderLight &l : blender_lights(s.lighting, view_dir, key_from_hdri, dim)) {
        bp_json lj;
        lj["name"]      = l.name;
        lj["type"]      = "SUN";
        lj["direction"] = bp_vec(l.direction);
        lj["color"]     = bp_vec(l.color);
        lj["strength"]  = bp_num(l.strength);
        lj["angle_deg"] = bp_num(l.angle_deg);
        lights.push_back(lj);
    }
    j["lights"] = lights;
    j["shadows"] = s.lighting.shadows;

    // Environment.
    {
        bp_json e;
        e["id"]           = s.lighting.environment;
        e["file"]         = hdri_path.empty() ? bp_json() : bp_json(hdri_path);
        e["rotation_deg"] = bp_num(s.lighting.env_rotation_deg);
        const float scale = in.has_hdri ? in.hdri.radiance_scale : 1.f;
        e["strength"]       = bp_num(std::max(0.f, s.lighting.env_intensity) * scale * std::clamp(1.f - dim, 0.f, 1.f));
        e["radiance_scale"] = bp_num(scale);
        e["intensity"]      = bp_num(s.lighting.env_intensity);
        e["sun_from_env"]   = key_from_hdri;
        if (in.has_hdri) {
            e["name"]    = in.hdri.name;
            e["source"]  = in.hdri.source;
            e["licence"] = in.hdri.licence;
        }
        // Without a panorama the world is the backdrop's lower colour, dimly.
        e["fallback_color"] = bp_vec(hex_to_linear(s.background.color2, { 0.8f, 0.8f, 0.8f }));
        j["environment"]    = e;
    }

    // Background.
    {
        const Background &bg = s.background;
        bp_json b;
        b["type"]           = to_string(bg.type);
        b["color1"]         = bp_vec(hex_to_linear(bg.color1));
        b["color2"]         = bp_vec(hex_to_linear(bg.color2));
        b["blur"]           = bp_num(bg.blur);
        b["image"]          = background_path.empty() ? bp_json() : bp_json(background_path);
        b["fit"]            = to_string(bg.fit);
        b["backdrop_pitch"] = bp_num(bg.backdrop_pitch);
        if (bg.type == BackgroundType::Backplate && !in.background_image.empty() && in.background_image.height > 0) {
            const double image_aspect = double(in.background_image.width) / double(in.background_image.height);
            const PlateMapping pm = plate_mapping(image_aspect, aspect, bg.plate_zoom, bg.plate_offset_x, bg.plate_offset_y);
            b["plate"]["sx"]      = bp_num(pm.sx);
            b["plate"]["sy"]      = bp_num(pm.sy);
            b["plate"]["ox"]      = bp_num(pm.ox);
            b["plate"]["oy"]      = bp_num(pm.oy);
            b["plate"]["horizon"] = bp_num(bg.plate_horizon);
            b["plate"]["focal"]   = bp_num(bg.plate_focal);
        }
        bp_json g;
        g["enabled"] = ground_projection_active(s);
        if (ground_projection_active(s)) {
            const GroundDome dome = ground_dome(bg, in.hdri.ground_height_mm, in.hdri.ground_radius_mm, in.model_min, in.model_max);
            g["center"]       = bp_vec(blender_from_mm(dome.center));
            g["height"]       = bp_num(dome.height * BLENDER_MM_TO_M);
            g["radius"]       = bp_num(dome.radius * BLENDER_MM_TO_M);
            g["floor_radius"] = bp_num(dome.floor_radius() * BLENDER_MM_TO_M);
        }
        b["ground"]     = g;
        j["background"] = b;
    }

    // Floor.
    {
        const Floor &f = s.floor;
        bp_json fl;
        fl["type"]            = to_string(effective_floor_type(s));
        fl["chosen_type"]     = to_string(f.type);
        fl["z"]               = 0.;
        fl["center"]          = bp_vec(blender_from_mm({ centre_mm[0], centre_mm[1], 0. }));
        fl["size"]            = bp_num(std::max(2., 40. * diag_mm * BLENDER_MM_TO_M));
        fl["color"]           = bp_vec(hex_to_linear(f.color));
        fl["shadow_strength"] = bp_num(f.shadow_strength);
        fl["grid_spacing"]    = bp_num(f.grid_spacing * BLENDER_MM_TO_M);
        fl["image"]           = floor_path.empty() ? bp_json() : bp_json(floor_path);
        fl["tile"]            = bp_num((f.type == FloorType::Image ? f.texture_size : in.floor_tile_mm * f.texture_scale) * BLENDER_MM_TO_M);
        fl["rotation"]        = f.rotation;
        fl["flip"]            = f.flip;
        fl["roughness"]       = bp_num(in.floor_roughness);
        fl["reflection"]      = bp_num(f.reflection);
        fl["contact_shadow"]  = bp_num(f.contact_shadow);
        j["floor"]            = fl;
    }

    // The rest of the look.
    {
        bp_json m;
        m["translucency"]       = bp_num(s.materials.translucency);
        m["layer_lines"]        = to_string(s.materials.layer_lines);
        m["layer_bump"]         = in.layer_bump && !in.toolpaths;
        m["lights_off"]         = s.materials.lights_off;
        m["lights_off_ambient"] = bp_num(s.materials.lights_off_ambient);
        m["show_internals"]     = in.show_internals;
        m["imperfections"]["wobble"]  = bp_num(s.materials.imperfections.wobble);
        m["imperfections"]["seam"]    = bp_num(s.materials.imperfections.seam);
        m["imperfections"]["strings"] = bp_num(s.materials.imperfections.strings);
        m["imperfections"]["colour"]  = bp_num(s.materials.imperfections.colour);
        j["look"]["materials"] = m;
        bp_json lens;
        lens["grain"]      = bp_num(s.lens.grain);
        lens["vignette"]   = bp_num(s.lens.vignette);
        lens["bloom"]      = bp_num(s.lens.bloom);
        lens["chromatic"]  = bp_num(s.lens.chromatic);
        lens["distortion"] = bp_num(s.lens.distortion);
        j["look"]["lens"]  = lens;
        if (in.toolpaths) {
            j["toolpaths"]["sides"]     = in.tubes.sides;
            j["toolpaths"]["runs"]      = in.tubes.runs;
            j["toolpaths"]["culled"]    = in.tubes.culled;
            j["toolpaths"]["dropped"]   = in.tubes.dropped;
            j["toolpaths"]["triangles"] = in.tubes.triangles;
        }
    }
    j["model"]["min"]    = bp_vec(blender_from_mm(in.model_min));
    j["model"]["max"]    = bp_vec(blender_from_mm(in.model_max));
    j["model"]["center"] = bp_vec(blender_from_mm(centre_mm));
    j["overlay"]         = overlay_path.empty() ? bp_json() : bp_json(overlay_path);
    j["camera_path"]     = in.frames.empty() ? bp_json() : bp_json("camera_path.json");

    out.files.push_back({ "showcase.json", bp_bytes(j.dump(2) + "\n") });

    // --- camera_path.json -------------------------------------------------------------------------
    if (!in.frames.empty()) {
        bp_json p;
        p["format"]  = BLENDER_CAMERA_PATH_FORMAT;
        p["version"] = BLENDER_PACKAGE_VERSION;
        p["fps"]     = in.fps;
        p["turntable_center"] = bp_vec(blender_from_mm({ centre_mm[0], centre_mm[1], 0. }));
        bp_json frames = bp_json::array();
        for (size_t i = 0; i < in.frames.size(); ++i) {
            const PackageFrame &f = in.frames[i];
            const BlenderCamera c = blender_camera(f.pose, s.camera.projection, frame_h, aspect);
            bp_json fj;
            fj["location"]     = bp_vec(c.location);
            fj["target"]       = bp_vec(c.target);
            fj["up"]           = bp_vec(c.up);
            fj["fov_y_deg"]    = bp_num(c.fov_y_deg);
            fj["lens_mm"]      = bp_num(c.lens_mm);
            fj["ortho_height"] = bp_num(c.ortho_height);
            fj["spin_deg"]     = bp_num(f.pose.spin_deg);
            fj["build_height"] = f.build_height_mm < 0. ? bp_json() : bp_json(bp_num(f.build_height_mm * BLENDER_MM_TO_M));
            frames.push_back(fj);
        }
        p["frames"] = frames;
        out.files.push_back({ "camera_path.json", bp_bytes(p.dump(1) + "\n") });
    }

    out.files.push_back({ "README.txt", bp_bytes(package_readme(in.edgeslicer_version)) });
    std::sort(out.files.begin(), out.files.end(), [](const PackageFile &a, const PackageFile &b) { return a.path < b.path; });

    char line[256];
    std::snprintf(line, sizeof(line), "%zu %s, %zu triangles, %zu materials%s%s", out.nodes, in.toolpaths ? "toolpath meshes" : "parts",
                  out.triangles, doc.materials.size(), hdri_path.empty() ? "" : ", HDRI", in.frames.empty() ? "" : ", camera path");
    out.summary = line;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Files
// ------------------------------------------------------------------------------------------------

bool write_package_folder(const std::vector<PackageFile> &files, const std::string &dir, std::string *error)
{
    namespace fs = boost::filesystem;
    try {
        const fs::path root(dir);
        fs::create_directories(root);
        for (const PackageFile &f : files) {
            if (f.path.empty() || f.path.find("..") != std::string::npos || f.path[0] == '/')
                return bp_fail(error, "a bad file name in the package: " + f.path);
            const fs::path target = root / fs::path(f.path);
            fs::create_directories(target.parent_path());
            boost::nowide::ofstream o(target.string(), std::ios::binary | std::ios::trunc);
            if (!o)
                return bp_fail(error, "cannot write " + target.string());
            o.write(reinterpret_cast<const char *>(f.bytes.data()), std::streamsize(f.bytes.size()));
            if (!o.good())
                return bp_fail(error, "cannot write " + target.string());
        }
    } catch (const std::exception &e) {
        return bp_fail(error, e.what());
    }
    return true;
}

namespace {
// Fixed time stamps (2024-01-01), so the same files give the same zip.
constexpr MZ_TIME_T BP_ZIP_TIME = MZ_TIME_T(1704067200);
} // namespace

bool write_package_zip(const std::vector<PackageFile> &files, const std::string &zip_path, std::string *error)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_writer(&zip, zip_path))
        return bp_fail(error, "cannot create " + zip_path);
    bool ok = true;
    for (const PackageFile &f : files) {
        MZ_TIME_T t = BP_ZIP_TIME;
        // HDRIs and pictures are compressed already.
        const bool stored = f.path.rfind("environment/", 0) == 0 || f.path.rfind("textures/", 0) == 0;
        if (!mz_zip_writer_add_mem_ex_v2(&zip, f.path.c_str(), f.bytes.data(), f.bytes.size(), nullptr, 0,
                                         stored ? MZ_NO_COMPRESSION : MZ_DEFAULT_COMPRESSION, 0, 0, &t, nullptr, 0, nullptr, 0)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = mz_zip_writer_finalize_archive(&zip);
    close_zip_writer(&zip);
    if (!ok) {
        boost::system::error_code ec;
        boost::filesystem::remove(zip_path, ec);
        return bp_fail(error, "cannot write " + zip_path);
    }
    return true;
}

bool zip_folder(const std::string &folder, const std::string &prefix, const std::string &zip_path, std::string *error)
{
    namespace fs = boost::filesystem;
    std::vector<PackageFile> files;
    try {
        const fs::path root(folder);
        if (!fs::is_directory(root))
            return bp_fail(error, "no folder " + folder);
        for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
            const fs::path p = it->path();
            if (fs::is_directory(p)) {
                if (p.filename() == "__pycache__")
                    it.disable_recursion_pending();
                continue;
            }
            if (p.extension() == ".pyc")
                continue;
            std::string rel = fs::relative(p, root).generic_string();
            boost::nowide::ifstream f(p.string(), std::ios::binary);
            if (!f)
                return bp_fail(error, "cannot read " + p.string());
            PackageFile pf;
            pf.path = prefix.empty() ? rel : prefix + "/" + rel;
            pf.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            files.push_back(std::move(pf));
        }
    } catch (const std::exception &e) {
        return bp_fail(error, e.what());
    }
    std::sort(files.begin(), files.end(), [](const PackageFile &a, const PackageFile &b) { return a.path < b.path; });
    return write_package_zip(files, zip_path, error);
}

} // namespace Showcase
} // namespace Slic3r

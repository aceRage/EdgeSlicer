#include "ShowcaseAssets.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace Showcase {

using json = nlohmann::json;

namespace {

std::string str_or(const json &j, const char *key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

float float_or(const json &j, const char *key, float def)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return def;
    const float v = it->get<float>();
    return std::isfinite(v) ? v : def;
}

// A plain file name next to the manifest: no folders, no "..".
bool safe_file_name(const std::string &f)
{
    return !f.empty() && f.find('/') == std::string::npos && f.find('\\') == std::string::npos && f.find("..") == std::string::npos &&
           f.find(':') == std::string::npos;
}

bool vec3_of(const json &j, std::array<float, 3> &out)
{
    if (!j.is_array() || j.size() != 3)
        return false;
    for (size_t i = 0; i < 3; ++i) {
        if (!j[i].is_number())
            return false;
        out[i] = j[i].get<float>();
        if (!std::isfinite(out[i]))
            return false;
    }
    return true;
}

} // namespace

const EnvironmentInfo *EnvironmentManifest::find(const std::string &id) const
{
    for (const EnvironmentInfo &e : environments)
        if (e.id == id)
            return &e;
    return nullptr;
}

const FloorTextureInfo *FloorManifest::find(const std::string &id) const
{
    for (const FloorTextureInfo &f : floors)
        if (f.id == id)
            return &f;
    return nullptr;
}

bool parse_environment_manifest(const std::string &text, EnvironmentManifest &out, std::string *error)
{
    out = EnvironmentManifest();
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("environments") || !j["environments"].is_array()) {
        if (error)
            *error = "environments.json is not a manifest";
        return false;
    }
    EnvironmentManifest m;
    m.rgbm_range = std::clamp(int(float_or(j, "rgbm_range", 16.f)), 1, 1024);
    m.face_size  = std::clamp(int(float_or(j, "face_size", 128.f)), 1, 4096);
    m.levels     = std::clamp(int(float_or(j, "levels", 5.f)), 1, 12);
    // Every level must keep at least one texel a face.
    while (m.levels > 1 && (m.face_size >> (m.levels - 1)) < 1)
        --m.levels;
    for (const json &e : j["environments"]) {
        if (!e.is_object())
            continue;
        EnvironmentInfo info;
        info.id   = str_or(e, "id");
        info.name = str_or(e, "name");
        info.file = str_or(e, "file");
        if (info.id.empty() || !safe_file_name(info.file) || m.find(info.id) != nullptr)
            continue;
        if (info.name.empty())
            info.name = info.id;
        const json sh = e.contains("sh") ? e["sh"] : json();
        bool sh_ok = sh.is_array() && sh.size() == 9;
        for (size_t i = 0; sh_ok && i < 9; ++i)
            sh_ok = vec3_of(sh[i], info.sh[i]);
        if (!sh_ok)
            continue;   // no diffuse light: useless for lighting
        if (e.contains("sun_dir") && vec3_of(e["sun_dir"], info.sun_dir)) {
            const float len = std::sqrt(info.sun_dir[0] * info.sun_dir[0] + info.sun_dir[1] * info.sun_dir[1] + info.sun_dir[2] * info.sun_dir[2]);
            info.has_sun = len > 1e-6f;
            if (info.has_sun)
                for (float &c : info.sun_dir)
                    c /= len;
        }
        info.source  = str_or(e, "source");
        info.authors = str_or(e, "authors");
        info.licence = str_or(e, "licence");
        m.environments.push_back(std::move(info));
    }
    out = std::move(m);
    return true;
}

bool parse_floor_manifest(const std::string &text, FloorManifest &out, std::string *error)
{
    out = FloorManifest();
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("floors") || !j["floors"].is_array()) {
        if (error)
            *error = "floors.json is not a manifest";
        return false;
    }
    FloorManifest m;
    for (const json &e : j["floors"]) {
        if (!e.is_object())
            continue;
        FloorTextureInfo info;
        info.id   = str_or(e, "id");
        info.name = str_or(e, "name");
        info.file = str_or(e, "file");
        if (info.id.empty() || !safe_file_name(info.file) || m.find(info.id) != nullptr)
            continue;
        if (info.name.empty())
            info.name = info.id;
        info.tile_mm   = std::clamp(float_or(e, "tile_mm", 500.f), 10.f, 10000.f);
        info.roughness = std::clamp(float_or(e, "roughness", 0.6f), 0.f, 1.f);
        info.source    = str_or(e, "source");
        info.authors   = str_or(e, "authors");
        info.licence   = str_or(e, "licence");
        m.floors.push_back(std::move(info));
    }
    out = std::move(m);
    return true;
}

int env_atlas_width(int face_size) { return 6 * std::max(1, face_size); }

int env_atlas_height(int face_size, int levels)
{
    int h = 0;
    for (int l = 0; l < levels; ++l)
        h += std::max(1, face_size >> l);
    return h;
}

RectI env_atlas_face(int face_size, int levels, int level, int face)
{
    if (level < 0 || level >= levels || face < 0 || face >= 6 || face_size <= 0)
        return RectI();
    const int n = std::max(1, face_size >> level);
    int       y = 0;
    for (int l = 0; l < level; ++l)
        y += std::max(1, face_size >> l);
    return RectI{ face * n, y, n, n };
}

std::array<float, 3> decode_rgbm(uint8_t r, uint8_t g, uint8_t b, uint8_t a, float range)
{
    const float m = float(a) / 255.f * range / 255.f;
    return { float(r) * m, float(g) * m, float(b) * m };
}

std::array<float, 9> sh_basis(const std::array<float, 3> &n)
{
    const float x = n[0], y = n[1], z = n[2];
    return { 0.282095f,
             0.488603f * y,
             0.488603f * z,
             0.488603f * x,
             1.092548f * x * y,
             1.092548f * y * z,
             0.315392f * (3.f * z * z - 1.f),
             1.092548f * x * z,
             0.546274f * (x * x - y * y) };
}

std::array<float, 3> eval_sh(const std::array<std::array<float, 3>, 9> &sh, const std::array<float, 3> &n)
{
    const std::array<float, 9> y = sh_basis(n);
    std::array<float, 3>       out{ 0.f, 0.f, 0.f };
    for (size_t i = 0; i < 9; ++i)
        for (size_t c = 0; c < 3; ++c)
            out[c] += sh[i][c] * y[i];
    for (float &c : out)
        c = std::max(0.f, c);
    return out;
}

std::array<float, 3> world_to_cube(const std::array<float, 3> &w) { return { w[0], w[2], -w[1] }; }
std::array<float, 3> cube_to_world(const std::array<float, 3> &c) { return { c[0], -c[2], c[1] }; }

} // namespace Showcase
} // namespace Slic3r

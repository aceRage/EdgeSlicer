#include "ShowcaseScene.hpp"

#include "../ImageFill.hpp"   // image_fill_sha256_hex

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace Showcase {

using json = nlohmann::json;

// ------------------------------------------------------------------------------------------------
// Enum names (the JSON spellings; never translate them)
// ------------------------------------------------------------------------------------------------

namespace {

template<class E, size_t N> const char *enum_name(E v, const char *const (&names)[N])
{
    const int i = int(v);
    return (i >= 0 && size_t(i) < N) ? names[i] : names[0];
}

template<class E, size_t N> bool enum_parse(const std::string &s, E &v, const char *const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) {
            v = E(int(i));
            return true;
        }
    return false;
}

const char *const BACKGROUND_NAMES[] = { "solid", "vertical_gradient", "radial_gradient", "image", "transparent", "environment" };
const char *const FIT_NAMES[]        = { "fill", "fit", "tile" };
const char *const FLOOR_NAMES[]      = { "none", "shadow_catcher", "solid", "grid", "texture", "image" };
const char *const QUALITY_NAMES[]    = { "draft", "high", "best" };
const char *const FORMAT_NAMES[]     = { "png", "jpg", "webp" };
const char *const PROJECTION_NAMES[] = { "perspective", "ortho" };
const char *const ANCHOR_NAMES[]     = { "top_left", "top", "top_right", "left", "center", "right", "bottom_left", "bottom", "bottom_right" };
const char *const PRESET_NAMES[]     = { "studio_white", "dark_gradient", "build_plate", "desk_wood", "outdoor" };

} // namespace

const char *to_string(BackgroundType v) { return enum_name(v, BACKGROUND_NAMES); }
const char *to_string(ImageFit v) { return enum_name(v, FIT_NAMES); }
const char *to_string(FloorType v) { return enum_name(v, FLOOR_NAMES); }
const char *to_string(Quality v) { return enum_name(v, QUALITY_NAMES); }
const char *to_string(ImageFormat v) { return enum_name(v, FORMAT_NAMES); }
const char *to_string(Projection v) { return enum_name(v, PROJECTION_NAMES); }
const char *to_string(Anchor v) { return enum_name(v, ANCHOR_NAMES); }
const char *to_string(PresetId v) { return enum_name(v, PRESET_NAMES); }
bool from_string(const std::string &s, BackgroundType &v) { return enum_parse(s, v, BACKGROUND_NAMES); }
bool from_string(const std::string &s, ImageFit &v) { return enum_parse(s, v, FIT_NAMES); }
bool from_string(const std::string &s, FloorType &v) { return enum_parse(s, v, FLOOR_NAMES); }
bool from_string(const std::string &s, Quality &v) { return enum_parse(s, v, QUALITY_NAMES); }
bool from_string(const std::string &s, ImageFormat &v) { return enum_parse(s, v, FORMAT_NAMES); }
bool from_string(const std::string &s, Projection &v) { return enum_parse(s, v, PROJECTION_NAMES); }
bool from_string(const std::string &s, Anchor &v) { return enum_parse(s, v, ANCHOR_NAMES); }
bool from_string(const std::string &s, PresetId &v) { return enum_parse(s, v, PRESET_NAMES); }

// ------------------------------------------------------------------------------------------------
// Colours
// ------------------------------------------------------------------------------------------------

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex_color(const std::string &hex, std::array<float, 4> &rgba)
{
    if (hex.size() != 7 && hex.size() != 9)
        return false;
    if (hex[0] != '#')
        return false;
    std::array<float, 4> out{ 0.f, 0.f, 0.f, 1.f };
    for (size_t c = 0; c * 2 + 1 < hex.size(); ++c) {
        const int hi = hex_digit(hex[1 + c * 2]);
        const int lo = hex_digit(hex[2 + c * 2]);
        if (hi < 0 || lo < 0)
            return false;
        out[c] = float(hi * 16 + lo) / 255.f;
    }
    rgba = out;
    return true;
}

std::string to_hex_color(const std::array<float, 4> &rgba, bool with_alpha)
{
    char buf[16];
    auto byte = [](float f) { return int(std::lround(std::clamp(f, 0.f, 1.f) * 255.f)); };
    if (with_alpha)
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", byte(rgba[0]), byte(rgba[1]), byte(rgba[2]), byte(rgba[3]));
    else
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(rgba[0]), byte(rgba[1]), byte(rgba[2]));
    return buf;
}

std::string normalize_hex_color(const std::string &hex, const std::string &fallback)
{
    std::array<float, 4> c;
    if (!parse_hex_color(hex, c))
        return fallback;
    std::string out = hex;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
    return out;
}

std::array<double, 3> light_direction(float azimuth_deg, float elevation_deg)
{
    static constexpr double DEG = 3.14159265358979323846 / 180.;
    const double     az  = double(azimuth_deg) * DEG;
    const double     el  = double(std::clamp(elevation_deg, -89.9f, 89.9f)) * DEG;
    return { std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el) };
}

std::vector<LightSource> three_point_lights(const Lighting &li)
{
    std::vector<LightSource> out;
    auto add = [&out](float az, float el, const std::array<float, 3> &rgb, float intensity) {
        if (!(intensity > 0.f))
            return;
        const std::array<double, 3> d = light_direction(az, el);
        LightSource l;
        l.dir   = { float(d[0]), float(d[1]), float(d[2]) };
        l.color = { rgb[0] * intensity, rgb[1] * intensity, rgb[2] * intensity };
        out.push_back(l);
    };
    add(li.azimuth_deg, li.elevation_deg, color_temperature_rgb(li.key_temperature), li.key_intensity);
    // Fill: across from the key, low, a touch cooler so the shadow side does not go grey.
    if (li.fill)
        add(li.azimuth_deg + 110.f, 20.f, color_temperature_rgb(7000.f), li.fill_intensity);
    // Rim: behind the subject as the key sees it, high, neutral; it outlines the silhouette.
    if (li.rim)
        add(li.azimuth_deg + 165.f, 50.f, color_temperature_rgb(6500.f), li.rim_intensity);
    return out;
}

std::vector<LightRig> light_rigs()
{
    return { LightRig::SoftThreePoint, LightRig::ClassicThreePoint, LightRig::Dramatic, LightRig::KeyOnly, LightRig::Sun };
}

void apply_light_rig(Lighting &li, LightRig rig)
{
    switch (rig) {
    default:
    case LightRig::SoftThreePoint:
        li.elevation_deg = 55.f; li.key_intensity = 1.f;  li.fill = true;  li.fill_intensity = 0.35f; li.rim = true;  li.rim_intensity = 0.45f;
        li.shadow_softness = 1.5f;
        break;
    case LightRig::ClassicThreePoint:
        li.elevation_deg = 45.f; li.key_intensity = 1.f;  li.fill = true;  li.fill_intensity = 0.5f;  li.rim = true;  li.rim_intensity = 0.7f;
        li.shadow_softness = 1.f;
        break;
    case LightRig::Dramatic:
        li.elevation_deg = 35.f; li.key_intensity = 1.15f; li.fill = false; li.rim = true;  li.rim_intensity = 1.f;
        li.shadow_softness = 1.f;
        break;
    case LightRig::KeyOnly:
        li.key_intensity = 1.1f; li.fill = false; li.rim = false;
        break;
    case LightRig::Sun:
        li.elevation_deg = 50.f; li.key_intensity = 1.3f; li.fill = false; li.rim = false;
        li.shadow_softness = 0.5f;
        break;
    }
}

// ------------------------------------------------------------------------------------------------
// Equality
// ------------------------------------------------------------------------------------------------

bool Background::operator==(const Background &o) const
{
    return type == o.type && color1 == o.color1 && color2 == o.color2 && image == o.image && fit == o.fit && blur == o.blur;
}

bool Floor::operator==(const Floor &o) const
{
    return type == o.type && color == o.color && shadow_strength == o.shadow_strength && grid_spacing == o.grid_spacing &&
           hide_plate == o.hide_plate && hide_wipe_tower == o.hide_wipe_tower && texture == o.texture && image == o.image &&
           texture_scale == o.texture_scale && texture_size == o.texture_size && reflection == o.reflection &&
           contact_shadow == o.contact_shadow;
}

bool Lighting::operator==(const Lighting &o) const
{
    return world_fixed == o.world_fixed && azimuth_deg == o.azimuth_deg && elevation_deg == o.elevation_deg && shadows == o.shadows &&
           shadow_intensity == o.shadow_intensity && ssao == o.ssao && key_intensity == o.key_intensity &&
           key_temperature == o.key_temperature && fill == o.fill && fill_intensity == o.fill_intensity && rim == o.rim &&
           rim_intensity == o.rim_intensity && environment == o.environment && env_intensity == o.env_intensity &&
           env_rotation_deg == o.env_rotation_deg && exposure == o.exposure && shadow_softness == o.shadow_softness;
}

bool CameraView::operator==(const CameraView &o) const
{
    return name == o.name && projection == o.projection && eye == o.eye && target == o.target && up == o.up && zoom == o.zoom &&
           valid == o.valid;
}

bool Watermark::operator==(const Watermark &o) const
{
    return name == o.name && enabled == o.enabled && text == o.text && font_face == o.font_face && bold == o.bold &&
           text_color == o.text_color && logo == o.logo && anchor == o.anchor && size_pct == o.size_pct && opacity == o.opacity &&
           margin_pct == o.margin_pct;
}

bool ExportSettings::operator==(const ExportSettings &o) const
{
    return width == o.width && height == o.height && quality == o.quality && format == o.format && jpeg_quality == o.jpeg_quality &&
           watermark == o.watermark && use_as_cover == o.use_as_cover;
}

bool Scene::operator==(const Scene &o) const
{
    return id == o.id && name == o.name && plate_index == o.plate_index && source == o.source && preset_base == o.preset_base &&
           background == o.background && floor == o.floor && lighting == o.lighting && camera == o.camera && views == o.views &&
           export_settings == o.export_settings && materials == o.materials;
}

const Finish *Materials::override_for(int filament) const
{
    for (const MaterialOverride &m : overrides)
        if (m.filament == filament)
            return &m.finish;
    return nullptr;
}

void Materials::set_override(int filament, const Finish *finish)
{
    overrides.erase(std::remove_if(overrides.begin(), overrides.end(), [filament](const MaterialOverride &m) { return m.filament == filament; }),
                    overrides.end());
    if (finish != nullptr && filament >= 1) {
        overrides.push_back({ filament, *finish });
        std::sort(overrides.begin(), overrides.end(), [](const MaterialOverride &a, const MaterialOverride &b) { return a.filament < b.filament; });
    }
}

Finish Materials::finish_for(int filament, const std::string &preset_name, const std::string &filament_type) const
{
    if (const Finish *f = override_for(filament))
        return *f;
    return infer_finish(preset_name, filament_type);
}

std::vector<std::string> Scene::used_images() const
{
    std::vector<std::string> out;
    if (background.type == BackgroundType::Image && !background.image.empty())
        out.push_back(background.image);
    if (floor.type == FloorType::Image && !floor.image.empty())
        out.push_back(floor.image);
    if (export_settings.watermark.enabled && !export_settings.watermark.logo.empty())
        out.push_back(export_settings.watermark.logo);
    return out;
}

bool Project::operator==(const Project &o) const
{
    return schema == o.schema && scenes == o.scenes && active == o.active && images == o.images;
}

// ------------------------------------------------------------------------------------------------
// Project
// ------------------------------------------------------------------------------------------------

void Project::clear()
{
    schema = SCHEMA_VERSION;
    scenes.clear();
    active.clear();
    images.clear();
    extra_json.clear();
    original_text.clear();
}

Scene *Project::scene_for_plate(int plate_index)
{
    for (Scene &s : scenes)
        if (s.plate_index == plate_index)
            return &s;
    return nullptr;
}

const Scene *Project::scene_for_plate(int plate_index) const
{
    for (const Scene &s : scenes)
        if (s.plate_index == plate_index)
            return &s;
    return nullptr;
}

Scene &Project::ensure_scene_for_plate(int plate_index, PresetId preset)
{
    if (Scene *s = scene_for_plate(plate_index))
        return *s;
    scenes.push_back(make_scene(preset, plate_index));
    // Unique id: "plate<N>", with a suffix should a stale scene already use it.
    std::string id = "plate" + std::to_string(plate_index + 1);
    for (int n = 2; std::any_of(scenes.begin(), scenes.end() - 1, [&id](const Scene &s) { return s.id == id; }); ++n)
        id = "plate" + std::to_string(plate_index + 1) + "_" + std::to_string(n);
    scenes.back().id = id;
    return scenes.back();
}

std::string Project::add_image(std::vector<uint8_t> bytes, const std::string &ext)
{
    if (bytes.empty())
        return {};
    const std::string sha = image_fill_sha256_hex(bytes);
    ImageBlob &blob = images[sha];
    if (blob.bytes.empty()) {
        blob.ext   = (ext == "jpg" || ext == "jpeg") ? "jpg" : "png";
        blob.bytes = std::move(bytes);
    }
    return sha;
}

const ImageBlob *Project::find_image(const std::string &sha) const
{
    auto it = images.find(sha);
    return it == images.end() ? nullptr : &it->second;
}

std::vector<std::string> Project::used_images() const
{
    std::vector<std::string> out;
    for (const Scene &s : scenes)
        for (const std::string &sha : s.used_images())
            out.push_back(sha);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Project::prune_images()
{
    const std::vector<std::string> used = used_images();
    for (auto it = images.begin(); it != images.end();) {
        if (!std::binary_search(used.begin(), used.end(), it->first))
            it = images.erase(it);
        else
            ++it;
    }
}

void Project::on_plate_deleted(int plate_index)
{
    if (read_only())
        return;
    scenes.erase(std::remove_if(scenes.begin(), scenes.end(), [plate_index](const Scene &s) { return s.plate_index == plate_index; }),
                 scenes.end());
    for (Scene &s : scenes)
        if (s.plate_index > plate_index)
            --s.plate_index;
}

// ------------------------------------------------------------------------------------------------
// JSON
// ------------------------------------------------------------------------------------------------

namespace {

template<class T> T get_or(const json &j, const char *key, const T &def)
{
    if (!j.is_object())
        return def;
    auto it = j.find(key);
    if (it == j.end())
        return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

template<class E> E enum_or(const json &j, const char *key, E def)
{
    E v = def;
    const std::string s = get_or<std::string>(j, key, std::string());
    return (!s.empty() && from_string(s, v)) ? v : def;
}

std::array<double, 3> vec3_or(const json &j, const char *key, const std::array<double, 3> &def)
{
    if (!j.is_object())
        return def;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3)
        return def;
    std::array<double, 3> v;
    for (size_t i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number())
            return def;
        v[i] = (*it)[i].get<double>();
        if (!std::isfinite(v[i]))
            return def;
    }
    return v;
}

float clamp_or(float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }

// Writes `known` into a copy of `raw` (when that is an object), so keys this build does not know
// survive inside the sections too.
json merged(const json &raw, const char *key, const json &known)
{
    json out = json::object();
    if (raw.is_object()) {
        auto it = raw.find(key);
        if (it != raw.end() && it->is_object())
            out = *it;
    }
    for (auto it = known.begin(); it != known.end(); ++it)
        out[it.key()] = it.value();
    return out;
}

json camera_to_json(const CameraView &c, const json &raw)
{
    json k;
    if (!c.name.empty())
        k["name"] = c.name;
    k["projection"] = to_string(c.projection);
    k["eye"]        = c.eye;
    k["target"]     = c.target;
    k["up"]         = c.up;
    k["zoom"]       = c.zoom;
    json out = raw.is_object() ? raw : json::object();
    for (auto it = k.begin(); it != k.end(); ++it)
        out[it.key()] = it.value();
    return out;
}

CameraView camera_from_json(const json &j)
{
    CameraView c;
    if (!j.is_object())
        return c;
    c.name       = get_or<std::string>(j, "name", "");
    c.projection = enum_or(j, "projection", Projection::Perspective);
    c.eye        = vec3_or(j, "eye", c.eye);
    c.target     = vec3_or(j, "target", c.target);
    c.up         = vec3_or(j, "up", c.up);
    c.zoom       = get_or<double>(j, "zoom", 1.);
    if (!std::isfinite(c.zoom) || c.zoom <= 0.)
        c.zoom = 1.;
    // A camera looking at its own eye has no direction; leave it for "frame the model".
    const double dx = c.eye[0] - c.target[0], dy = c.eye[1] - c.target[1], dz = c.eye[2] - c.target[2];
    c.valid = dx * dx + dy * dy + dz * dz > 1e-12;
    return c;
}

json watermark_to_json(const Watermark &w, const json &raw)
{
    json k;
    k["name"]       = w.name;
    k["enabled"]    = w.enabled;
    k["text"]       = w.text;
    k["font"]       = w.font_face;
    k["bold"]       = w.bold;
    k["text_color"] = w.text_color;
    k["logo"]       = w.logo;
    k["anchor"]     = to_string(w.anchor);
    k["size_pct"]   = w.size_pct;
    k["opacity"]    = w.opacity;
    k["margin_pct"] = w.margin_pct;
    json out = raw.is_object() ? raw : json::object();
    for (auto it = k.begin(); it != k.end(); ++it)
        out[it.key()] = it.value();
    return out;
}

Watermark watermark_from_json(const json &j)
{
    Watermark w;
    if (!j.is_object())
        return w;
    w.name       = get_or<std::string>(j, "name", "");
    w.enabled    = get_or<bool>(j, "enabled", false);
    w.text       = get_or<std::string>(j, "text", "");
    w.font_face  = get_or<std::string>(j, "font", "");
    w.bold       = get_or<bool>(j, "bold", false);
    w.text_color = normalize_hex_color(get_or<std::string>(j, "text_color", w.text_color), "#FFFFFF");
    w.logo       = get_or<std::string>(j, "logo", "");
    w.anchor     = enum_or(j, "anchor", Anchor::BottomRight);
    w.size_pct   = clamp_or(get_or<float>(j, "size_pct", w.size_pct), 0.5f, 50.f, 6.f);
    w.opacity    = clamp_or(get_or<float>(j, "opacity", w.opacity), 0.f, 1.f, 0.85f);
    w.margin_pct = clamp_or(get_or<float>(j, "margin_pct", w.margin_pct), 0.f, 25.f, 3.f);
    return w;
}

json scene_to_json_object(const Scene &s)
{
    json raw = json::object();
    if (!s.extra_json.empty()) {
        raw = json::parse(s.extra_json, nullptr, false);
        if (!raw.is_object())
            raw = json::object();
    }
    json j = raw;
    j["id"]          = s.id;
    j["name"]        = s.name;
    j["plate_index"] = s.plate_index;
    j["source"]      = s.source;
    j["preset_base"] = s.preset_base;

    json bg;
    bg["type"]   = to_string(s.background.type);
    bg["color1"] = s.background.color1;
    bg["color2"] = s.background.color2;
    bg["image"]  = s.background.image;
    bg["fit"]    = to_string(s.background.fit);
    bg["blur"]   = s.background.blur;
    j["background"] = merged(raw, "background", bg);

    json fl;
    fl["type"]            = to_string(s.floor.type);
    fl["color"]           = s.floor.color;
    fl["shadow_strength"] = s.floor.shadow_strength;
    fl["grid_spacing"]    = s.floor.grid_spacing;
    fl["hide_plate"]      = s.floor.hide_plate;
    fl["hide_wipe_tower"] = s.floor.hide_wipe_tower;
    fl["texture"]         = s.floor.texture;
    fl["image"]           = s.floor.image;
    fl["texture_scale"]   = s.floor.texture_scale;
    fl["texture_size"]    = s.floor.texture_size;
    fl["reflection"]      = s.floor.reflection;
    fl["contact_shadow"]  = s.floor.contact_shadow;
    j["floor"] = merged(raw, "floor", fl);

    json li;
    li["world_fixed"]      = s.lighting.world_fixed;
    li["azimuth"]          = s.lighting.azimuth_deg;
    li["elevation"]        = s.lighting.elevation_deg;
    li["shadows"]          = s.lighting.shadows;
    li["shadow_intensity"] = s.lighting.shadow_intensity;
    li["ssao"]             = s.lighting.ssao;
    li["key_intensity"]    = s.lighting.key_intensity;
    li["key_temperature"]  = s.lighting.key_temperature;
    li["fill"]             = s.lighting.fill;
    li["fill_intensity"]   = s.lighting.fill_intensity;
    li["rim"]              = s.lighting.rim;
    li["rim_intensity"]    = s.lighting.rim_intensity;
    li["environment"]      = s.lighting.environment;
    li["env_intensity"]    = s.lighting.env_intensity;
    li["env_rotation"]     = s.lighting.env_rotation_deg;
    li["exposure"]         = s.lighting.exposure;
    li["shadow_softness"]  = s.lighting.shadow_softness;
    j["lighting"] = merged(raw, "lighting", li);

    if (s.camera.valid)
        j["camera"] = camera_to_json(s.camera, raw.is_object() && raw.contains("camera") ? raw["camera"] : json());
    else
        j.erase("camera");

    json views = json::array();
    for (const CameraView &v : s.views)
        if (v.valid)
            views.push_back(camera_to_json(v, json()));
    j["views"] = views;

    json ex;
    ex["width"]        = s.export_settings.width;
    ex["height"]       = s.export_settings.height;
    ex["quality"]      = to_string(s.export_settings.quality);
    ex["format"]       = to_string(s.export_settings.format);
    ex["jpeg_quality"] = s.export_settings.jpeg_quality;
    ex["use_as_cover"] = s.export_settings.use_as_cover;
    json raw_export = raw.is_object() && raw.contains("export") ? raw["export"] : json();
    ex["watermark"] = watermark_to_json(s.export_settings.watermark,
                                        raw_export.is_object() && raw_export.contains("watermark") ? raw_export["watermark"] : json());
    j["export"] = merged(raw, "export", ex);

    json ma;
    ma["layer_lines"] = to_string(s.materials.layer_lines);
    json filaments = json::array();
    for (const MaterialOverride &m : s.materials.overrides)
        filaments.push_back({ { "slot", m.filament }, { "finish", to_string(m.finish) } });
    ma["filaments"] = filaments;
    j["materials"] = merged(raw, "materials", ma);
    return j;
}

bool scene_from_json_object(const json &j, Scene &s)
{
    if (!j.is_object())
        return false;
    s             = Scene();
    s.extra_json  = j.dump();
    s.id          = get_or<std::string>(j, "id", "");
    s.name        = get_or<std::string>(j, "name", "");
    s.plate_index = std::max(0, get_or<int>(j, "plate_index", 0));
    s.source      = get_or<std::string>(j, "source", "model");
    if (s.source != "model" && s.source != "toolpaths")
        s.source = "model";
    s.preset_base = get_or<std::string>(j, "preset_base", "");

    const json bg       = j.contains("background") ? j["background"] : json();
    s.background.type   = enum_or(bg, "type", s.background.type);
    s.background.color1 = normalize_hex_color(get_or<std::string>(bg, "color1", s.background.color1), s.background.color1);
    s.background.color2 = normalize_hex_color(get_or<std::string>(bg, "color2", s.background.color2), s.background.color2);
    s.background.image  = get_or<std::string>(bg, "image", "");
    s.background.fit    = enum_or(bg, "fit", s.background.fit);
    s.background.blur   = clamp_or(get_or<float>(bg, "blur", s.background.blur), 0.f, 1.f, 0.f);

    const json fl             = j.contains("floor") ? j["floor"] : json();
    s.floor.type              = enum_or(fl, "type", s.floor.type);
    s.floor.color             = normalize_hex_color(get_or<std::string>(fl, "color", s.floor.color), s.floor.color);
    s.floor.shadow_strength   = clamp_or(get_or<float>(fl, "shadow_strength", s.floor.shadow_strength), 0.f, 1.f, 0.45f);
    s.floor.grid_spacing      = clamp_or(get_or<float>(fl, "grid_spacing", s.floor.grid_spacing), 1.f, 1000.f, 10.f);
    s.floor.hide_plate        = get_or<bool>(fl, "hide_plate", s.floor.hide_plate);
    s.floor.hide_wipe_tower   = get_or<bool>(fl, "hide_wipe_tower", s.floor.hide_wipe_tower);
    s.floor.texture           = get_or<std::string>(fl, "texture", s.floor.texture);
    s.floor.image             = get_or<std::string>(fl, "image", "");
    s.floor.texture_scale     = clamp_or(get_or<float>(fl, "texture_scale", s.floor.texture_scale), 0.1f, 10.f, 1.f);
    s.floor.texture_size      = clamp_or(get_or<float>(fl, "texture_size", s.floor.texture_size), 10.f, 5000.f, 300.f);
    s.floor.reflection        = clamp_or(get_or<float>(fl, "reflection", s.floor.reflection), 0.f, 1.f, 0.f);
    s.floor.contact_shadow    = clamp_or(get_or<float>(fl, "contact_shadow", s.floor.contact_shadow), 0.f, 1.f, 0.6f);

    const json li               = j.contains("lighting") ? j["lighting"] : json();
    s.lighting.world_fixed      = get_or<bool>(li, "world_fixed", s.lighting.world_fixed);
    s.lighting.azimuth_deg      = clamp_or(get_or<float>(li, "azimuth", s.lighting.azimuth_deg), -360.f, 720.f, 225.f);
    s.lighting.elevation_deg    = clamp_or(get_or<float>(li, "elevation", s.lighting.elevation_deg), 1.f, 90.f, 55.f);
    s.lighting.shadows          = get_or<bool>(li, "shadows", s.lighting.shadows);
    s.lighting.shadow_intensity = clamp_or(get_or<float>(li, "shadow_intensity", s.lighting.shadow_intensity), 0.f, 1.f, 0.35f);
    s.lighting.ssao             = get_or<bool>(li, "ssao", s.lighting.ssao);
    s.lighting.key_intensity    = clamp_or(get_or<float>(li, "key_intensity", s.lighting.key_intensity), 0.f, 3.f, 1.f);
    s.lighting.key_temperature  = clamp_or(get_or<float>(li, "key_temperature", s.lighting.key_temperature), 2000.f, 10000.f, 5600.f);
    s.lighting.fill             = get_or<bool>(li, "fill", s.lighting.fill);
    s.lighting.fill_intensity   = clamp_or(get_or<float>(li, "fill_intensity", s.lighting.fill_intensity), 0.f, 3.f, 0.35f);
    s.lighting.rim              = get_or<bool>(li, "rim", s.lighting.rim);
    s.lighting.rim_intensity    = clamp_or(get_or<float>(li, "rim_intensity", s.lighting.rim_intensity), 0.f, 3.f, 0.45f);
    s.lighting.environment      = get_or<std::string>(li, "environment", s.lighting.environment);
    s.lighting.env_intensity    = clamp_or(get_or<float>(li, "env_intensity", s.lighting.env_intensity), 0.f, 3.f, 0.7f);
    s.lighting.env_rotation_deg = clamp_or(get_or<float>(li, "env_rotation", s.lighting.env_rotation_deg), -360.f, 720.f, 0.f);
    s.lighting.exposure         = clamp_or(get_or<float>(li, "exposure", s.lighting.exposure), -3.f, 3.f, 0.f);
    s.lighting.shadow_softness  = clamp_or(get_or<float>(li, "shadow_softness", s.lighting.shadow_softness), 0.5f, 4.f, 1.f);

    if (j.contains("camera"))
        s.camera = camera_from_json(j["camera"]);
    if (j.contains("views") && j["views"].is_array())
        for (const json &v : j["views"]) {
            CameraView c = camera_from_json(v);
            if (c.valid)
                s.views.push_back(std::move(c));
        }

    const json ex                  = j.contains("export") ? j["export"] : json();
    s.export_settings.width        = clamp_export_side(get_or<int>(ex, "width", s.export_settings.width));
    s.export_settings.height       = clamp_export_side(get_or<int>(ex, "height", s.export_settings.height));
    s.export_settings.quality      = enum_or(ex, "quality", s.export_settings.quality);
    s.export_settings.format       = enum_or(ex, "format", s.export_settings.format);
    s.export_settings.jpeg_quality = std::clamp(get_or<int>(ex, "jpeg_quality", s.export_settings.jpeg_quality), 10, 100);
    if (ex.is_object() && ex.contains("watermark"))
        s.export_settings.watermark = watermark_from_json(ex["watermark"]);
    s.export_settings.use_as_cover = get_or<bool>(ex, "use_as_cover", false);   // D10: never on unless saved on

    const json ma              = j.contains("materials") ? j["materials"] : json();
    s.materials.layer_lines    = enum_or(ma, "layer_lines", s.materials.layer_lines);
    if (ma.is_object() && ma.contains("filaments") && ma["filaments"].is_array())
        for (const json &f : ma["filaments"]) {
            const int slot = get_or<int>(f, "slot", 0);
            Finish    finish;
            if (slot >= 1 && slot <= 1024 && from_string(get_or<std::string>(f, "finish", ""), finish))
                s.materials.set_override(slot, &finish);
        }
    return true;
}

} // namespace

std::string scene_to_json(const Scene &scene) { return scene_to_json_object(scene).dump(); }

std::string watermark_to_json(const Watermark &wm) { return watermark_to_json(wm, json()).dump(); }

bool watermark_from_json(const std::string &text, Watermark &out)
{
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object())
        return false;
    out = watermark_from_json(j);
    return true;
}

bool scene_from_json(const std::string &text, Scene &out)
{
    const json j = json::parse(text, nullptr, false);
    return scene_from_json_object(j, out);
}

std::string scenes_to_json(const Project &project)
{
    if (project.read_only() && !project.original_text.empty())
        return project.original_text;
    json j = json::object();
    if (!project.extra_json.empty()) {
        j = json::parse(project.extra_json, nullptr, false);
        if (!j.is_object())
            j = json::object();
    }
    j["schema"] = SCHEMA_VERSION;
    j["active"] = project.active;
    json scenes = json::array();
    for (const Scene &s : project.scenes)
        scenes.push_back(scene_to_json_object(s));
    j["scenes"] = scenes;
    return j.dump(2);
}

bool scenes_from_json(const std::string &text, Project &out, std::string *error)
{
    Project p;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        if (error)
            *error = "scenes.json is not a JSON object";
        out = Project();
        return false;
    }
    p.schema = get_or<int>(j, "schema", 0);
    if (p.schema <= 0) {
        if (error)
            *error = "scenes.json has no valid schema number";
        out = Project();
        return false;
    }
    p.extra_json = j.dump();
    p.active     = get_or<std::string>(j, "active", "");
    if (j.contains("scenes") && j["scenes"].is_array())
        for (const json &sj : j["scenes"]) {
            Scene s;
            if (scene_from_json_object(sj, s))
                p.scenes.push_back(std::move(s));
        }
    if (p.read_only())
        p.original_text = text;
    out = std::move(p);
    return true;
}

std::string image_archive_path(const std::string &sha, const std::string &ext)
{
    return std::string(ARCHIVE_DIR) + sha + "." + (ext == "jpg" ? "jpg" : "png");
}

bool parse_image_archive_path(const std::string &path, std::string &sha, std::string &ext)
{
    const std::string dir = ARCHIVE_DIR;
    if (path.size() <= dir.size() || path.compare(0, dir.size(), dir) != 0)
        return false;
    const std::string name = path.substr(dir.size());
    const size_t      dot  = name.find_last_of('.');
    if (dot == std::string::npos || dot != 64)
        return false;
    std::string e = name.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (e == "jpeg")
        e = "jpg";
    if (e != "png" && e != "jpg")
        return false;
    const std::string s = name.substr(0, dot);
    for (char c : s)
        if (hex_digit(c) < 0)
            return false;
    sha = s;
    std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    ext = e;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Presets
// ------------------------------------------------------------------------------------------------

std::vector<PresetId> builtin_presets()
{
    return { PresetId::StudioWhite, PresetId::DeskWood, PresetId::DarkGradient, PresetId::Outdoor, PresetId::BuildPlate };
}

void apply_preset(Scene &scene, PresetId preset)
{
    scene.preset_base = to_string(preset);
    Background &bg = scene.background;
    Floor      &fl = scene.floor;
    Lighting   &li = scene.lighting;
    // The look only: the floor picture, the materials, the camera and the export stay the user's.
    const std::string floor_image = fl.image;
    bg = Background();
    fl = Floor();
    li = Lighting();
    fl.image = floor_image;
    switch (preset) {
    default:
    case PresetId::StudioWhite:
        // Product shot: white sweep, soft key + fill + rim, studio softboxes in the reflections.
        bg.type   = BackgroundType::RadialGradient;
        bg.color1 = "#FFFFFF";
        bg.color2 = "#D9DCE1";
        fl.type   = FloorType::ShadowCatcher;
        fl.color  = "#F2F2F2";
        fl.shadow_strength = 0.45f;
        break;
    case PresetId::DeskWood:
        // Maker's desk: an oak top, a warm window key, the workshop around it softly out of focus.
        bg.type   = BackgroundType::Environment;
        bg.blur   = 0.7f;
        bg.color1 = "#E8DCCB";
        bg.color2 = "#8E7A63";
        fl.type   = FloorType::Texture;
        fl.texture = "oak";
        fl.shadow_strength = 0.5f;
        fl.contact_shadow  = 0.7f;
        li.azimuth_deg     = 200.f;
        li.elevation_deg   = 40.f;
        li.key_temperature = 4000.f;
        li.key_intensity   = 1.1f;
        li.fill_intensity  = 0.25f;
        li.rim_intensity   = 0.3f;
        li.environment     = "workshop";
        li.env_intensity   = 0.8f;
        li.shadow_softness = 1.5f;
        break;
    case PresetId::DarkGradient:
        // Dramatic: a single key and a strong rim on charcoal, no fill.
        bg.type   = BackgroundType::VerticalGradient;
        bg.color1 = "#3A3D42";   // top
        bg.color2 = "#0B0C0E";   // bottom
        fl.type   = FloorType::ShadowCatcher;
        fl.color  = "#1A1B1E";
        fl.shadow_strength = 0.6f;
        li.azimuth_deg      = 200.f;
        li.elevation_deg    = 40.f;
        li.shadow_intensity = 0.45f;
        li.key_intensity    = 1.15f;
        li.fill             = false;
        li.rim_intensity    = 1.f;
        li.environment      = "dark_studio";
        li.env_intensity    = 0.5f;
        break;
    case PresetId::Outdoor:
        // Daylight: the sky behind, concrete under, a hard sun and the sky as the fill.
        bg.type   = BackgroundType::Environment;
        bg.blur   = 0.35f;
        bg.color1 = "#BFD6EE";
        bg.color2 = "#E9EEF2";
        fl.type   = FloorType::Texture;
        fl.texture = "concrete";
        fl.shadow_strength = 0.55f;
        li.azimuth_deg      = 135.f;
        li.elevation_deg    = 50.f;
        li.key_temperature  = 5800.f;
        li.key_intensity    = 1.3f;
        li.shadow_intensity = 0.45f;
        li.shadow_softness  = 0.5f;
        li.fill             = false;
        li.rim              = false;
        li.environment      = "outdoor";
        li.env_intensity    = 1.f;
        break;
    case PresetId::BuildPlate:
        // "As on the printer": the real plate, the editing view's light.
        bg.type   = BackgroundType::VerticalGradient;
        bg.color1 = "#F4F5F7";
        bg.color2 = "#C9CDD3";
        fl.type   = FloorType::None;
        fl.hide_plate      = false;
        fl.hide_wipe_tower = false;
        li.env_intensity   = 0.5f;
        break;
    }
}

Scene make_scene(PresetId preset, int plate_index)
{
    Scene s;
    s.plate_index = plate_index;
    s.id          = "plate" + std::to_string(plate_index + 1);
    s.name        = "Plate " + std::to_string(plate_index + 1);
    apply_preset(s, preset);
    return s;
}

// ------------------------------------------------------------------------------------------------
// Export planning and the safe frame
// ------------------------------------------------------------------------------------------------

int ssaa_factor(Quality q)
{
    switch (q) {
    case Quality::Draft: return 1;
    case Quality::Best: return 3;
    default:
    case Quality::High: return 2;
    }
}

int clamp_export_side(int v) { return std::clamp(v, MIN_EXPORT_SIDE, MAX_EXPORT_SIDE); }

size_t export_memory_estimate(int internal_w, int internal_h, int msaa)
{
    const double px = double(internal_w) * double(internal_h);
    // MSAA colour (RGBA8) + depth (D24S8) per sample, a resolved colour + depth, two SSAO copies
    // and the CPU-side read back.
    const double per_px = (msaa > 0 ? 8. * double(msaa) : 0.) + 8. + 8. + 4.;
    return size_t(px * per_px);
}

ExportPlan plan_export(int width, int height, Quality quality, int max_msaa, int reference_height, size_t budget_bytes, int max_texture_size)
{
    ExportPlan p;
    const int side_cap = std::min(MAX_EXPORT_SIDE, std::max(MIN_EXPORT_SIDE, max_texture_size));
    p.width  = std::clamp(width, MIN_EXPORT_SIDE, side_cap);
    p.height = std::clamp(height, MIN_EXPORT_SIDE, side_cap);
    int ssaa = ssaa_factor(quality);
    // D11: the internal image obeys the same cap.
    while (ssaa > 1 && (p.width * ssaa > side_cap || p.height * ssaa > side_cap)) {
        --ssaa;
        p.degraded = true;
    }
    int msaa = std::clamp(max_msaa, 0, 4);
    if (msaa == 1)
        msaa = 0;
    // Memory: drop MSAA first (supersampling already smooths edges), then the supersampling.
    for (;;) {
        const size_t bytes = export_memory_estimate(p.width * ssaa, p.height * ssaa, msaa);
        if (bytes <= budget_bytes)
            break;
        if (msaa > 0) {
            msaa = 0;
            p.degraded = true;
        } else if (ssaa > 1) {
            --ssaa;
            p.degraded = true;
        } else
            break;
    }
    p.ssaa            = ssaa;
    p.msaa            = msaa;
    p.internal_width  = p.width * ssaa;
    p.internal_height = p.height * ssaa;
    p.estimated_bytes = double(export_memory_estimate(p.internal_width, p.internal_height, msaa));
    p.ssao_radius_scale = reference_height > 0 ? std::max(1.f, float(p.internal_height) / float(reference_height)) : 1.f;
    p.shadow_map_size   = unsigned(std::min(4096, std::max(1024, max_texture_size)));
    return p;
}

RectI fit_frame(int viewport_w, int viewport_h, int aspect_w, int aspect_h, int margin_px)
{
    RectI r;
    const int avail_w = viewport_w - 2 * margin_px;
    const int avail_h = viewport_h - 2 * margin_px;
    if (avail_w <= 0 || avail_h <= 0 || aspect_w <= 0 || aspect_h <= 0)
        return r;
    // Integer arithmetic: the frame's aspect matches the export's to within one pixel.
    if (int64_t(avail_w) * aspect_h <= int64_t(avail_h) * aspect_w) {
        r.w = avail_w;
        r.h = std::max(1, int((int64_t(avail_w) * aspect_h + aspect_w / 2) / aspect_w));
    } else {
        r.h = avail_h;
        r.w = std::max(1, int((int64_t(avail_h) * aspect_w + aspect_h / 2) / aspect_h));
    }
    r.x = margin_px + (avail_w - r.w) / 2;
    r.y = margin_px + (avail_h - r.h) / 2;
    return r;
}

// ------------------------------------------------------------------------------------------------
// Watermark
// ------------------------------------------------------------------------------------------------

int watermark_font_px(const Watermark &wm, int image_h, bool has_logo)
{
    const double block = double(image_h) * double(wm.size_pct) / 100.;
    // With a logo the text is set a little smaller than the logo is tall, so the two read as one mark.
    return std::max(6, int(std::lround(block * (has_logo ? 0.5 : 0.75))));
}

WatermarkLayout layout_watermark(const Watermark &wm, int image_w, int image_h, double logo_aspect, int text_w, int text_h)
{
    WatermarkLayout out;
    const bool has_logo = logo_aspect > 0.;
    const bool has_text = text_w > 0 && text_h > 0;
    out.font_px = watermark_font_px(wm, image_h, has_logo);
    if ((!has_logo && !has_text) || image_w <= 0 || image_h <= 0)
        return out;

    const int block_h_target = std::max(1, int(std::lround(double(image_h) * double(wm.size_pct) / 100.)));
    int logo_w = 0, logo_h = 0;
    if (has_logo) {
        logo_h = block_h_target;
        logo_w = std::max(1, int(std::lround(double(logo_h) * logo_aspect)));
    }
    const int gap     = (has_logo && has_text) ? std::max(1, int(std::lround(0.3 * double(logo_h)))) : 0;
    int       block_w = logo_w + gap + (has_text ? text_w : 0);
    int       block_h = std::max(logo_h, has_text ? text_h : 0);

    // A mark wider than the image is scaled down to fit (very long text on a small export).
    const int margin = std::max(0, int(std::lround(double(std::min(image_w, image_h)) * double(wm.margin_pct) / 100.)));
    const int max_w  = std::max(1, image_w - 2 * margin);
    const int max_h  = std::max(1, image_h - 2 * margin);
    double    scale  = 1.;
    if (block_w > max_w)
        scale = std::min(scale, double(max_w) / double(block_w));
    if (block_h > max_h)
        scale = std::min(scale, double(max_h) / double(block_h));
    if (scale < 1.) {
        logo_w  = int(std::floor(logo_w * scale));
        logo_h  = int(std::floor(logo_h * scale));
        text_w  = int(std::floor(text_w * scale));
        text_h  = int(std::floor(text_h * scale));
        out.font_px = std::max(1, int(std::floor(out.font_px * scale)));
        const int g = int(std::floor(gap * scale));
        block_w = logo_w + g + (has_text ? text_w : 0);
        block_h = std::max(logo_h, has_text ? text_h : 0);
    }
    const int gap_scaled = block_w - logo_w - (has_text ? text_w : 0);

    // Anchor: column 0/1/2 (left, centre, right), row 0/1/2 (top, middle, bottom).
    const int a   = int(wm.anchor);
    const int col = a % 3;
    const int row = a / 3;
    int x = col == 0 ? margin : col == 1 ? (image_w - block_w) / 2 : image_w - margin - block_w;
    int y = row == 0 ? margin : row == 1 ? (image_h - block_h) / 2 : image_h - margin - block_h;
    x = std::clamp(x, 0, std::max(0, image_w - block_w));
    y = std::clamp(y, 0, std::max(0, image_h - block_h));

    out.block = { x, y, block_w, block_h };
    if (has_logo)
        out.logo = { x, y + (block_h - logo_h) / 2, logo_w, logo_h };
    if (has_text)
        out.text = { x + logo_w + gap_scaled, y + (block_h - text_h) / 2, text_w, text_h };
    return out;
}

// ------------------------------------------------------------------------------------------------
// Image helpers
// ------------------------------------------------------------------------------------------------

std::vector<uint8_t> downsample_rgba(const std::vector<uint8_t> &src, int w, int h, int factor)
{
    if (factor <= 1)
        return src;
    const int ow = w / factor, oh = h / factor;
    std::vector<uint8_t> out(size_t(ow) * size_t(oh) * 4);
    const int n = factor * factor;
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            unsigned sum[4] = { 0, 0, 0, 0 };
            for (int dy = 0; dy < factor; ++dy) {
                const uint8_t *row = src.data() + (size_t(y * factor + dy) * size_t(w) + size_t(x * factor)) * 4;
                for (int dx = 0; dx < factor; ++dx)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += row[dx * 4 + c];
            }
            uint8_t *o = out.data() + (size_t(y) * size_t(ow) + size_t(x)) * 4;
            for (int c = 0; c < 4; ++c)
                o[c] = uint8_t((sum[c] + unsigned(n / 2)) / unsigned(n));
        }
    return out;
}

void flip_rows_rgba(std::vector<uint8_t> &rgba, int w, int h)
{
    const size_t stride = size_t(w) * 4;
    std::vector<uint8_t> tmp(stride);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t *a = rgba.data() + size_t(y) * stride;
        uint8_t *b = rgba.data() + size_t(h - 1 - y) * stride;
        std::copy(a, a + stride, tmp.data());
        std::copy(b, b + stride, a);
        std::copy(tmp.data(), tmp.data() + stride, b);
    }
}

void unpremultiply_rgba(std::vector<uint8_t> &rgba)
{
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        if (a == 0 || a == 255)
            continue;
        for (int c = 0; c < 3; ++c)
            rgba[i + c] = uint8_t(std::min(255u, (unsigned(rgba[i + c]) * 255u + a / 2) / a));
    }
}

void blend_over_rgba(std::vector<uint8_t> &dst, int dst_w, int dst_h, const std::vector<uint8_t> &src, int src_w, int src_h,
                     int x, int y, float opacity)
{
    const float op = std::clamp(opacity, 0.f, 1.f);
    if (op <= 0.f)
        return;
    for (int sy = 0; sy < src_h; ++sy) {
        const int dy = y + sy;
        if (dy < 0 || dy >= dst_h)
            continue;
        for (int sx = 0; sx < src_w; ++sx) {
            const int dx = x + sx;
            if (dx < 0 || dx >= dst_w)
                continue;
            const uint8_t *s  = src.data() + (size_t(sy) * size_t(src_w) + size_t(sx)) * 4;
            uint8_t       *d  = dst.data() + (size_t(dy) * size_t(dst_w) + size_t(dx)) * 4;
            const float    sa = float(s[3]) / 255.f * op;
            if (sa <= 0.f)
                continue;
            const float da = float(d[3]) / 255.f;
            const float oa = sa + da * (1.f - sa);
            for (int c = 0; c < 3; ++c) {
                const float v = (float(s[c]) * sa + float(d[c]) * da * (1.f - sa)) / std::max(oa, 1e-6f);
                d[c] = uint8_t(std::lround(std::clamp(v, 0.f, 255.f)));
            }
            d[3] = uint8_t(std::lround(std::clamp(oa * 255.f, 0.f, 255.f)));
        }
    }
}

void flatten_rgba(std::vector<uint8_t> &rgba, uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t bg[3] = { r, g, b };
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        for (int c = 0; c < 3; ++c)
            rgba[i + c] = uint8_t((unsigned(rgba[i + c]) * a + unsigned(bg[c]) * (255u - a) + 127u) / 255u);
        rgba[i + 3] = 255;
    }
}

} // namespace Showcase
} // namespace Slic3r

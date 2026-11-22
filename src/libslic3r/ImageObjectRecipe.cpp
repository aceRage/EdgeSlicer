#include "ImageObjectRecipe.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>

#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

using nlohmann::json;

const int    RECIPE_VERSION       = 1;
const size_t RECIPE_MAX_NAME      = 200;
const int    RECIPE_MAX_INDEX     = 15;
const size_t RECIPE_MAX_OBJECTS   = 10000;

const char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_of(const std::string &bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        uint32_t v = (uint32_t(uint8_t(bytes[i])) << 16) | (uint32_t(uint8_t(bytes[i + 1])) << 8) | uint32_t(uint8_t(bytes[i + 2]));
        out += BASE64_CHARS[(v >> 18) & 63];
        out += BASE64_CHARS[(v >> 12) & 63];
        out += BASE64_CHARS[(v >> 6) & 63];
        out += BASE64_CHARS[v & 63];
    }
    if (i + 1 == bytes.size()) {
        uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
        out += BASE64_CHARS[(v >> 18) & 63];
        out += BASE64_CHARS[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        uint32_t v = (uint32_t(uint8_t(bytes[i])) << 16) | (uint32_t(uint8_t(bytes[i + 1])) << 8);
        out += BASE64_CHARS[(v >> 18) & 63];
        out += BASE64_CHARS[(v >> 12) & 63];
        out += BASE64_CHARS[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// Empty for text which is not base64 or which decodes to more than max_bytes
std::string bytes_of_base64(const std::string &text, size_t max_bytes)
{
    if (text.size() % 4 != 0 || text.size() / 4 * 3 > max_bytes + 2)
        return {};
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int        pad  = 0;
        uint32_t   v    = 0;
        for (size_t k = 0; k < 4; ++k) {
            char c = text[i + k];
            if (c == '=') {
                // padding only at the very end
                if (!last || k < 2)
                    return {};
                ++pad;
                v <<= 6;
                continue;
            }
            int d = value(c);
            if (d < 0 || pad > 0)
                return {};
            v = (v << 6) | uint32_t(d);
        }
        out += char((v >> 16) & 255);
        if (pad < 2)
            out += char((v >> 8) & 255);
        if (pad < 1)
            out += char(v & 255);
    }
    if (out.size() > max_bytes)
        return {};
    return out;
}

void read_double(const json &j, const char *key, double &out, double min, double max)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return;
    double v = it->get<double>();
    if (std::isfinite(v))
        out = std::clamp(v, min, max);
}

void read_int(const json &j, const char *key, int &out, int min, int max)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return;
    double v = it->get<double>();
    if (std::isfinite(v))
        out = int(std::clamp(std::round(v), double(min), double(max)));
}

void read_bool(const json &j, const char *key, bool &out)
{
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean())
        out = it->get<bool>();
}

template<class T> void limit(T &value, T min, T max)
{
    // also replaces a value which is not a number
    value = value >= min ? (value <= max ? value : max) : min;
}

json settings_json(const ImageToObjectSettings &s)
{
    json j;
    j["version"]        = RECIPE_VERSION;
    j["type"]           = int(s.type);
    j["shape_from"]     = int(s.shape_from);
    j["threshold"]      = s.threshold;
    j["auto_threshold"] = s.auto_threshold;
    j["invert"]         = s.invert;
    j["blur"]           = s.blur;
    j["size"]           = s.silhouette.longest_side;
    j["close_gaps"]     = s.silhouette.close_gaps;
    j["min_feature"]    = s.silhouette.min_feature;
    j["fill_holes"]     = s.silhouette.fill_holes;
    j["largest_only"]   = s.silhouette.largest_only;
    j["mirror"]         = s.cutter_mirror;

    json &c              = j["cutter"];
    c["blade_height"]    = s.cutter.blade_height;
    c["blade_thickness"] = s.cutter.blade_thickness;
    c["handle_width"]    = s.cutter.handle_width;
    c["handle_height"]   = s.cutter.handle_height;
    c["sharp_edge"]      = s.cutter.sharp_edge;
    c["tip_width"]       = s.cutter.tip_width;
    c["tip_height"]      = s.cutter.tip_height;
    c["taper_step"]      = s.cutter.taper_step;
    c["braces"]          = int(s.cutter.braces);
    c["brace_width"]     = s.cutter.brace_width;
    c["brace_spacing"]   = s.cutter.brace_spacing;

    json &b                = j["box"];
    b["height"]            = s.box.height;
    b["wall"]              = s.box.wall;
    b["floor"]             = s.box.floor;
    b["chamfer"]           = s.box.chamfer;
    b["lid_thickness"]     = s.box.lid_thickness;
    b["lid_overhang"]      = s.box.lid_overhang;
    b["lip_height"]        = s.box.lip_height;
    b["lip_thickness"]     = s.box.lip_thickness;
    b["clearance"]         = s.box.clearance;
    b["decoration_depth"]  = s.box.decoration_depth;
    b["decoration_margin"] = s.box.decoration_margin;

    json &k                = j["clicker"];
    k["body_height"]       = s.clicker.body_height;
    k["floor"]             = s.clicker.floor;
    k["chamfer"]           = s.clicker.chamfer;
    k["socket_fit"]        = s.clicker.socket_fit;
    k["stem_fit"]          = s.clicker.stem_fit;
    k["switch_x"]          = s.clicker.switch_offset.x();
    k["switch_y"]          = s.clicker.switch_offset.y();
    k["cap_thickness"]     = s.clicker.cap_thickness;
    k["cap_overhang"]      = s.clicker.cap_overhang;
    k["decoration_depth"]  = s.clicker.decoration_depth;
    k["decoration_margin"] = s.clicker.decoration_margin;
    k["keyring"]           = s.clicker.keyring;
    k["keyring_angle"]     = s.clicker.keyring_angle;

    j["picture_colors"] = s.picture_colors;
    j["body_filament"]  = s.body_extruder;
    json levels         = json::array();
    for (int e : s.level_extruders)
        levels.push_back(e);
    j["level_filaments"] = std::move(levels);
    return j;
}

bool settings_from(const json &j, ImageToObjectSettings &s)
{
    if (!j.is_object())
        return false;
    int type = int(s.type), shape_from = int(s.shape_from);
    read_int(j, "type", type, 0, 2);
    read_int(j, "shape_from", shape_from, 0, 2);
    s.type       = ImageObjectType(type);
    s.shape_from = TraceShapeFrom(shape_from);
    read_int(j, "threshold", s.threshold, 0, 255);
    read_bool(j, "auto_threshold", s.auto_threshold);
    read_bool(j, "invert", s.invert);
    read_double(j, "blur", s.blur, 0., 5.);
    read_double(j, "size", s.silhouette.longest_side, 10., 1000.);
    read_double(j, "close_gaps", s.silhouette.close_gaps, 0., 5.);
    read_double(j, "min_feature", s.silhouette.min_feature, 0., 10.);
    read_bool(j, "fill_holes", s.silhouette.fill_holes);
    read_bool(j, "largest_only", s.silhouette.largest_only);
    read_bool(j, "mirror", s.cutter_mirror);

    if (auto it = j.find("cutter"); it != j.end() && it->is_object()) {
        const json &c = *it;
        read_double(c, "blade_height", s.cutter.blade_height, 3., 100.);
        read_double(c, "blade_thickness", s.cutter.blade_thickness, 0.3, 5.);
        read_double(c, "handle_width", s.cutter.handle_width, 0.3, 30.);
        read_double(c, "handle_height", s.cutter.handle_height, 0.4, 20.);
        read_bool(c, "sharp_edge", s.cutter.sharp_edge);
        read_double(c, "tip_width", s.cutter.tip_width, 0.1, 5.);
        read_double(c, "tip_height", s.cutter.tip_height, 0.2, 20.);
        read_double(c, "taper_step", s.cutter.taper_step, 0.05, 2.);
        int braces = int(s.cutter.braces);
        read_int(c, "braces", braces, 0, 2);
        s.cutter.braces = CutterBraces(braces);
        read_double(c, "brace_width", s.cutter.brace_width, 0.4, 20.);
        read_double(c, "brace_spacing", s.cutter.brace_spacing, 5., 500.);
    }
    if (auto it = j.find("box"); it != j.end() && it->is_object()) {
        const json &b = *it;
        read_double(b, "height", s.box.height, 3., 300.);
        read_double(b, "wall", s.box.wall, 0.4, 10.);
        read_double(b, "floor", s.box.floor, 0.2, 10.);
        read_double(b, "chamfer", s.box.chamfer, 0., 5.);
        read_double(b, "lid_thickness", s.box.lid_thickness, 0.4, 20.);
        read_double(b, "lid_overhang", s.box.lid_overhang, 0., 10.);
        read_double(b, "lip_height", s.box.lip_height, 0.4, 50.);
        read_double(b, "lip_thickness", s.box.lip_thickness, 0.4, 10.);
        read_double(b, "clearance", s.box.clearance, 0., 1.);
        read_double(b, "decoration_depth", s.box.decoration_depth, 0.1, 5.);
        read_double(b, "decoration_margin", s.box.decoration_margin, 0., 10.);
    }
    if (auto it = j.find("clicker"); it != j.end() && it->is_object()) {
        const json &k = *it;
        read_double(k, "body_height", s.clicker.body_height, 9., 60.);
        read_double(k, "floor", s.clicker.floor, 0.2, 10.);
        read_double(k, "chamfer", s.clicker.chamfer, 0., 5.);
        read_double(k, "socket_fit", s.clicker.socket_fit, -0.2, 0.5);
        read_double(k, "stem_fit", s.clicker.stem_fit, -0.1, 0.3);
        double x = s.clicker.switch_offset.x(), y = s.clicker.switch_offset.y();
        read_double(k, "switch_x", x, -200., 200.);
        read_double(k, "switch_y", y, -200., 200.);
        s.clicker.switch_offset = Vec2d(x, y);
        read_double(k, "cap_thickness", s.clicker.cap_thickness, 0.4, 20.);
        read_double(k, "cap_overhang", s.clicker.cap_overhang, 0., 10.);
        read_double(k, "decoration_depth", s.clicker.decoration_depth, 0.1, 5.);
        read_double(k, "decoration_margin", s.clicker.decoration_margin, 0., 10.);
        read_bool(k, "keyring", s.clicker.keyring);
        read_double(k, "keyring_angle", s.clicker.keyring_angle, -360., 360.);
    }

    read_int(j, "picture_colors", s.picture_colors, 0, IMAGE_OBJECT_MAX_COLORS);
    read_int(j, "body_filament", s.body_extruder, 0, 255);
    if (auto it = j.find("level_filaments"); it != j.end() && it->is_array())
        for (size_t i = 0; i < it->size() && i < s.level_extruders.size(); ++i)
            if ((*it)[i].is_number()) {
                double v = (*it)[i].get<double>();
                if (std::isfinite(v))
                    s.level_extruders[i] = int(std::clamp(std::round(v), 0., 255.));
            }
    clamp_image_object_settings(s);
    return true;
}

} // namespace

void clamp_image_object_settings(ImageToObjectSettings &s)
{
    s.type       = ImageObjectType(std::clamp(int(s.type), 0, 2));
    s.shape_from = TraceShapeFrom(std::clamp(int(s.shape_from), 0, 2));
    limit(s.threshold, 0, 255);
    limit(s.blur, 0., 5.);
    limit(s.silhouette.longest_side, 10., 1000.);
    limit(s.silhouette.close_gaps, 0., 5.);
    limit(s.silhouette.min_feature, 0., 10.);
    limit(s.cutter.blade_height, 3., 100.);
    limit(s.cutter.blade_thickness, 0.3, 5.);
    limit(s.cutter.handle_width, 0.3, 30.);
    limit(s.cutter.handle_height, 0.4, 20.);
    s.cutter.braces = CutterBraces(std::clamp(int(s.cutter.braces), 0, 2));
    limit(s.box.height, 3., 300.);
    limit(s.box.wall, 0.4, 10.);
    limit(s.box.lid_overhang, 0., 10.);
    limit(s.box.clearance, 0., 1.);
    limit(s.clicker.body_height, 9., 60.);
    limit(s.clicker.socket_fit, -0.2, 0.5);
    limit(s.clicker.stem_fit, -0.1, 0.3);
    double x = s.clicker.switch_offset.x(), y = s.clicker.switch_offset.y();
    limit(x, -200., 200.);
    limit(y, -200., 200.);
    s.clicker.switch_offset = Vec2d(x, y);
    limit(s.clicker.cap_overhang, 0., 10.);
    limit(s.clicker.keyring_angle, -360., 360.);
    limit(s.picture_colors, 0, IMAGE_OBJECT_MAX_COLORS);
    if (s.picture_colors == 1)
        s.picture_colors = 0;
    limit(s.body_extruder, 0, 255);
    for (int &e : s.level_extruders)
        limit(e, 0, 255);

    // The box is seen from above as the image is; only the cutter is turned over
    s.silhouette.mirror  = s.type == ImageObjectType::CookieCutter && s.cutter_mirror;
    s.box.decoration     = s.picture_colors >= 2;
    s.clicker.decoration = s.picture_colors >= 2;
}

std::string image_object_settings_to_json(const ImageToObjectSettings &settings) { return settings_json(settings).dump(); }

bool image_object_settings_from_json(const std::string &text, ImageToObjectSettings &settings)
{
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded())
        return false;
    return settings_from(j, settings);
}

bool is_valid_image_object_group(const std::string &group)
{
    if (group.size() < 8 || group.size() > 64)
        return false;
    return std::all_of(group.begin(), group.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string new_image_object_group()
{
    static const char  hex[] = "0123456789abcdef";
    std::random_device device;
    std::mt19937_64    generator((uint64_t(device()) << 32) ^ uint64_t(device()));
    std::string        out;
    for (int word = 0; word < 2; ++word) {
        uint64_t v = generator();
        for (int i = 0; i < 16; ++i, v >>= 4)
            out += hex[v & 15];
    }
    return out;
}

std::string image_object_recipes_to_json(const std::vector<const ImageObjectRecipe *> &objects)
{
    json list   = json::array();
    json images = json::object();
    for (size_t i = 0; i < objects.size(); ++i) {
        const ImageObjectRecipe *recipe = objects[i];
        if (recipe == nullptr || !is_valid_image_object_group(recipe->group))
            continue;
        json settings = json::parse(recipe->settings, nullptr, false);
        if (settings.is_discarded() || !settings.is_object())
            continue;
        json entry;
        entry["object"]   = int(i) + 1;
        entry["group"]    = recipe->group;
        entry["index"]    = recipe->index;
        entry["name"]     = recipe->image_name;
        entry["settings"] = std::move(settings);
        list.push_back(std::move(entry));
        // the image of a group is written once
        if (!recipe->source_png.empty() && recipe->source_png.size() <= IMAGE_OBJECT_MAX_SOURCE_BYTES &&
            images.find(recipe->group) == images.end())
            images[recipe->group] = base64_of(recipe->source_png);
    }
    if (list.empty())
        return {};
    json root;
    root["version"] = RECIPE_VERSION;
    root["objects"] = std::move(list);
    root["images"]  = std::move(images);
    // a name with broken UTF-8 must not stop the project from being saved
    return root.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::vector<std::pair<int, ImageObjectRecipe>> image_object_recipes_from_json(const std::string &text)
{
    std::vector<std::pair<int, ImageObjectRecipe>> out;
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return out;
    auto list = root.find("objects");
    if (list == root.end() || !list->is_array())
        return out;

    std::map<std::string, std::string> images;
    if (auto it = root.find("images"); it != root.end() && it->is_object())
        for (auto image = it->begin(); image != it->end(); ++image)
            if (image.value().is_string() && is_valid_image_object_group(image.key())) {
                std::string png = bytes_of_base64(image.value().get<std::string>(), IMAGE_OBJECT_MAX_SOURCE_BYTES);
                if (!png.empty())
                    images.emplace(image.key(), std::move(png));
            }

    for (const json &entry : *list) {
        if (out.size() >= RECIPE_MAX_OBJECTS)
            break;
        if (!entry.is_object())
            continue;
        int object = 0;
        read_int(entry, "object", object, 0, 1000000);
        auto group    = entry.find("group");
        auto settings = entry.find("settings");
        if (object < 1 || group == entry.end() || !group->is_string() || settings == entry.end())
            continue;
        ImageObjectRecipe recipe;
        recipe.group = group->get<std::string>();
        if (!is_valid_image_object_group(recipe.group))
            continue;
        ImageToObjectSettings parsed;
        if (!settings_from(*settings, parsed))
            continue;
        // what is kept is what was understood
        recipe.settings = image_object_settings_to_json(parsed);
        read_int(entry, "index", recipe.index, 0, RECIPE_MAX_INDEX);
        if (auto name = entry.find("name"); name != entry.end() && name->is_string()) {
            recipe.image_name = name->get<std::string>();
            if (recipe.image_name.size() > RECIPE_MAX_NAME)
                recipe.image_name.clear();
        }
        if (auto image = images.find(recipe.group); image != images.end())
            recipe.source_png = image->second;
        if (std::any_of(out.begin(), out.end(), [object](const auto &o) { return o.first == object; }))
            continue;
        out.emplace_back(object, std::move(recipe));
    }
    return out;
}

} // namespace Slic3r

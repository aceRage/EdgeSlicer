#ifndef slic3r_CrealityPrintModel_hpp_
#define slic3r_CrealityPrintModel_hpp_

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "Http.hpp"

// Network-free Creality K-series helpers, so Catch2 can pin the /info model table and the
// WebSocket host parse without a printer. Port of upstream Orca #14089's is_k1_family() /
// cfs_capable_models() / Http::get_host_from_url usage; this fork's Http::url_host is the
// equivalent (scheme and port stripped).

namespace Slic3r {
namespace CrealityPrintModel {

// K1-family firmware stores gcodes under /usr/data and uses a stricter WebSocket
// protocol (TEXT frames, heartbeat acks). Verified K1-family /info "model" values.
inline bool is_k1_family(const std::string &model)
{
    static const std::unordered_set<std::string> k1_models = {
        "K1",
        "K1 SE",
        "K1C",
        "K1_CFS-C",
    };
    return k1_models.count(model) > 0;
}

// CFS-capable models. One table for capability checks and display names.
// K2-platform /info values are F-codes; K1-family values are the literal names.
inline const std::map<std::string, std::string> &cfs_capable_models()
{
    static const std::map<std::string, std::string> models = {
        {"F008", "K2 Plus"},
        {"F012", "K2 Pro"},
        {"F021", "K2"},
        {"F022", "SPARKX i7"},
        {"K1", "K1"},
        {"K1 SE", "K1 SE"},
        {"K1C", "K1C"},
        {"K1_CFS-C", "K1_CFS-C"},
    };
    return models;
}

inline bool model_supports_multi_color(const std::string &model) { return cfs_capable_models().count(model) > 0; }

inline std::string model_display_name(const std::string &model)
{
    auto &      names = cfs_capable_models();
    const auto  it    = names.find(model);
    return it != names.end() ? it->second : std::string{};
}

// Preset printer_model values are "Creality K1C", while /info uses "K1C".
inline std::string strip_vendor_prefix(const std::string &model)
{
    const std::string prefix = "Creality ";
    if (model.size() > prefix.size() && model.compare(0, prefix.size(), prefix) == 0)
        return model.substr(prefix.size());
    return model;
}

inline bool is_known_model(const std::string &model) { return is_k1_family(model) || model_supports_multi_color(model); }

// /info codes and preset names ("Creality K1", "K1 Max", "K1C_CFS-C").
inline bool looks_like_k1_family(const std::string &model)
{
    if (is_k1_family(model))
        return true;
    const std::string n = strip_vendor_prefix(model);
    if (is_k1_family(n))
        return true;
    return n.size() >= 2 && n[0] == 'K' && n[1] == '1';
}

// Prefer a known /info model. An empty or unknown /info value falls back to the
// preset's printer_model so a K1 never regresses to the K2 /mnt/UDISK root.
inline std::string resolve_model(const std::string &info_model, const std::string &preset_model)
{
    if (is_known_model(info_model))
        return info_model;
    const std::string n = strip_vendor_prefix(preset_model);
    if (is_known_model(n))
        return n;
    for (const auto &kv : cfs_capable_models())
        if (kv.second == n || kv.second == preset_model)
            return kv.first;
    if (!n.empty())
        return n;
    return info_model;
}

// K1-family: /usr/data. Everything else (K2 / K2 Pro / K2 Plus / SPARKX i7, and an
// unknown model with no K1-like preset fallback) uses /mnt/UDISK.
inline std::string gcode_root(const std::string &model) { return looks_like_k1_family(model) ? "/usr/data" : "/mnt/UDISK"; }

inline std::string gcode_root_for(const std::string &info_model, const std::string &preset_model)
{
    return gcode_root(resolve_model(info_model, preset_model));
}

inline std::string gcode_path(const std::string &model, const std::string &filename)
{
    return gcode_root(model) + "/printer_data/gcodes/" + filename;
}

// Upstream omits the multipart "path" field on CFS-capable models (CrealityPrint.cpp:158).
inline bool omit_upload_path(const std::string &model) { return model_supports_multi_color(model); }

// Strip scheme and port so the port-9999 WebSocket can resolve the host. This fork's
// Http::url_host is the equivalent of upstream Http::get_host_from_url for that job.
inline std::string ws_host(const std::string &print_host) { return Http::url_host(print_host); }

// The /info JSON's "model" field. Hostile or non-object bodies return "" and never throw.
inline std::string parse_model_from_info(const std::string &body)
{
    try {
        const nlohmann::json info = nlohmann::json::parse(body);
        if (info.is_object() && info.contains("model") && info["model"].is_string())
            return info["model"].get<std::string>();
    } catch (...) {}
    return {};
}

// CFS hardware variants Creality Print's device manager knows. The boxsInfo JSON may name
// them as a model code (MF003, ...) or a display string; unknown codes stay "CFS".
inline std::string cfs_variant_name(const std::string &code)
{
    static const std::map<std::string, std::string> variants = {
        {"MF003", "CFS"},
        {"CFS", "CFS"},
        {"MF050", "CFS-C"},
        {"CFS-C", "CFS-C"},
        {"MF040", "CFS Lite"},
        {"CFS Lite", "CFS Lite"},
        {"MF046", "CFS Mini"},
        {"CFS Mini", "CFS Mini"},
        {"MF049", "CFS Nano"},
        {"CFS Nano", "CFS Nano"},
        {"MF042", "CFS Pro"},
        {"CFS Pro", "CFS Pro"},
        {"CFS Nano2", "CFS Nano2"},
        {"MF051", "CFS Nano2"},
    };
    const auto it = variants.find(code);
    return it != variants.end() ? it->second : (code.empty() ? std::string("CFS") : code);
}

inline std::string colour_hex(std::string raw)
{
    if (!raw.empty() && raw[0] == '#')
        raw = raw.substr(1);
    // 8-character HTML colours (#RRGGBBAA): keep RGB, drop alpha.
    if (raw.size() >= 8) {
        for (size_t i = 0; i < 8; ++i)
            if (!std::isxdigit(static_cast<unsigned char>(raw[i])))
                return {};
        raw = raw.substr(0, 6);
    } else if (raw.size() == 7 && raw[0] == '0') {
        // Creality reports "#0RRGGBB" (leading zero after '#') on some firmwares.
        raw = raw.substr(1);
    }
    if (raw.size() < 6)
        return {};
    for (size_t i = 0; i < 6; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(raw[i])))
            return {};
    return "#" + raw.substr(0, 6);
}

inline bool iequals_ascii(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

struct CfsSlot
{
    int         box_id   = 0; // raw printer boxId (0 is the external spool holder)
    int         slot_id  = 0; // 0..3 inside a CFS box
    std::string color_hex;
    std::string filament_type;
    std::string brand_name;
    std::string vendor;
    std::string variant; // "CFS", "CFS-C", ...
    bool        loaded   = false;
    bool        external = false;
};

struct CfsParseResult
{
    std::vector<CfsSlot> slots;
    int                  box_count = 0; // active CFS boxes (type 0)
    std::string          variant;       // first CFS box's variant, if named
    std::string          error;
    bool                 ok = false;
};

inline int json_int(const nlohmann::json &j, const char *key, int fallback, int lo, int hi)
{
    if (!j.is_object() || !j.contains(key))
        return fallback;
    const nlohmann::json &v = j[key];
    int                   n = fallback;
    try {
        if (v.is_number_integer())
            n = v.get<int>();
        else if (v.is_number()) {
            const double d = v.get<double>();
            if (!std::isfinite(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi))
                return fallback;
            n = static_cast<int>(d);
        } else if (v.is_string())
            n = std::stoi(v.get<std::string>());
        else
            return fallback;
    } catch (...) {
        return fallback;
    }
    if (n < lo || n > hi)
        return fallback;
    return n;
}

inline std::string json_str(const nlohmann::json &j, const char *key)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string())
        return {};
    return j[key].get<std::string>();
}

// Parse boxsInfo from the port-9999 WebSocket. Caps boxes/slots, never throws.
// Schema (K2 Combo F021): { boxsInfo: { materialBoxs: [ { id, state, type, model?,
//   materials: [ { id, state, vendor, type, name, color } ] } ] } }
// type 0 = CFS, type 1 = external spool holder. Empty CFS slots stay in the list.
inline CfsParseResult parse_cfs_response(const std::string &response)
{
    CfsParseResult out;
    if (response.empty()) {
        out.error = "empty response";
        return out;
    }
    constexpr size_t kMaxMessage = 1024 * 1024;
    if (response.size() > kMaxMessage) {
        out.error = "message too large";
        return out;
    }
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(response);
    } catch (...) {
        out.error = "JSON parse error";
        return out;
    }
    if (!resp.is_object()) {
        out.error = "invalid schema";
        return out;
    }
    nlohmann::json boxes = nlohmann::json::array();
    try {
        if (resp.contains("boxsInfo") && resp["boxsInfo"].is_object() && resp["boxsInfo"].contains("materialBoxs") &&
            resp["boxsInfo"]["materialBoxs"].is_array())
            boxes = resp["boxsInfo"]["materialBoxs"];
        else if (resp.contains("result") && resp["result"].is_object() && resp["result"].contains("boxsInfo") &&
                 resp["result"]["boxsInfo"].is_object() && resp["result"]["boxsInfo"].contains("materialBoxs") &&
                 resp["result"]["boxsInfo"]["materialBoxs"].is_array())
            boxes = resp["result"]["boxsInfo"]["materialBoxs"];
        else {
            out.error = "invalid schema (missing boxsInfo.materialBoxs)";
            return out;
        }
    } catch (...) {
        out.error = "invalid schema";
        return out;
    }

    constexpr int kMaxBoxes = 8;
    constexpr int kMaxSlots = 32;
    int           seen      = 0;
    for (const auto &box : boxes) {
        if (!box.is_object())
            continue;
        if (seen >= kMaxBoxes)
            break;
        ++seen;
        const int box_st   = json_int(box, "state", 0, 0, 16);
        const int box_type = json_int(box, "type", 0, 0, 16);
        const int box_id   = json_int(box, "id", 0, 0, 64);
        if (box_st != 1)
            continue;
        std::string variant = json_str(box, "model");
        if (variant.empty())
            variant = json_str(box, "cfsType");
        if (variant.empty())
            variant = json_str(box, "typeName");
        variant = cfs_variant_name(variant);

        if (box_type == 1) {
            CfsSlot s;
            s.box_id   = box_id;
            s.slot_id  = 0;
            s.external = true;
            s.variant  = "External";
            if (box.contains("materials") && box["materials"].is_array() && !box["materials"].empty() &&
                box["materials"][0].is_object()) {
                const auto &mat = box["materials"][0];
                s.vendor        = json_str(mat, "vendor");
                s.filament_type = json_str(mat, "type");
                s.brand_name    = json_str(mat, "name");
                s.color_hex     = colour_hex(json_str(mat, "color"));
                const int st    = json_int(mat, "state", 0, 0, 16);
                s.loaded        = st != 0 && !(s.vendor.empty() && s.filament_type.empty());
            }
            if (static_cast<int>(out.slots.size()) < kMaxSlots)
                out.slots.push_back(std::move(s));
            continue;
        }
        if (box_type != 0)
            continue;

        ++out.box_count;
        if (out.variant.empty())
            out.variant = variant;

        nlohmann::json mats = nlohmann::json::array();
        if (box.contains("materials") && box["materials"].is_array())
            mats = box["materials"];

        // Four physical slots, even when the firmware omitted an empty one.
        CfsSlot by_id[4];
        bool    have[4] = {};
        for (const auto &mat : mats) {
            if (!mat.is_object())
                continue;
            const int sid = json_int(mat, "id", -1, 0, 3);
            if (sid < 0 || have[sid])
                continue;
            have[sid]               = true;
            by_id[sid].box_id       = box_id;
            by_id[sid].slot_id      = sid;
            by_id[sid].vendor       = json_str(mat, "vendor");
            by_id[sid].filament_type = json_str(mat, "type");
            by_id[sid].brand_name   = json_str(mat, "name");
            by_id[sid].color_hex    = colour_hex(json_str(mat, "color"));
            by_id[sid].variant      = variant;
            const int st            = json_int(mat, "state", 0, 0, 16);
            // K2/K2 Pro: 0 empty, 1 loaded. K2 Plus: 1 selected, 2 loaded.
            by_id[sid].loaded = st != 0 && !(by_id[sid].vendor.empty() && by_id[sid].filament_type.empty());
        }
        for (int i = 0; i < 4; ++i) {
            if (static_cast<int>(out.slots.size()) >= kMaxSlots)
                break;
            if (!have[i]) {
                CfsSlot empty;
                empty.box_id  = box_id;
                empty.slot_id = i;
                empty.variant = variant;
                out.slots.push_back(std::move(empty));
            } else {
                out.slots.push_back(std::move(by_id[i]));
            }
        }
    }
    out.ok = true;
    return out;
}

// G-code filament index, as Creality Print / upstream Orca:
// T{idx/4+1}{'A'+idx%4}. Destination box/slot travel in the mapped fields.
inline std::string color_match_id(int filament_index)
{
    if (filament_index < 0 || filament_index > 63)
        return {};
    return "T" + std::to_string(filament_index / 4 + 1) + std::string(1, static_cast<char>('A' + (filament_index % 4)));
}

// Tab-separated dialog / extendedInfo value. Tests and the send dialog share this
// so the id is never rebuilt beside the helper that produces it.
inline std::string color_match_value(int filament_index, const std::string &type, const std::string &color, int box_id, int material_id)
{
    const std::string id = color_match_id(filament_index);
    if (id.empty())
        return {};
    return id + "\t" + type + "\t" + color + "\t" + std::to_string(box_id) + "\t" + std::to_string(material_id);
}

inline nlohmann::json op_gcode_file_cmd(const std::string &gcode_path, int enable_self_test)
{
    return nlohmann::json{{"method", "set"},
                          {"params", {{"opGcodeFile", "printprt:" + gcode_path}, {"enableSelfTest", enable_self_test}}}};
}

inline nlohmann::json color_match_cmd(const std::string &gcode_path, const nlohmann::json &list)
{
    return nlohmann::json{{"method", "set"}, {"params", {{"colorMatch", {{"path", gcode_path}, {"list", list}}}}}};
}

inline nlohmann::json multi_color_print_cmd(const std::string &gcode_path, int enable_self_test)
{
    return nlohmann::json{{"method", "set"},
                          {"params", {{"multiColorPrint", {{"gcode", gcode_path}, {"enableSelfTest", enable_self_test}}}}}};
}

// Tab-separated dialog value: id, type, colour, boxId, materialId.
inline nlohmann::json color_match_item_from_value(const std::string &val)
{
    std::vector<std::string> parts;
    std::string              part;
    for (char c : val) {
        if (c == '\t') {
            parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    parts.push_back(part);
    if (parts.size() < 5)
        return {};
    int box_id = 0, material_id = 0;
    try {
        box_id      = std::stoi(parts[3]);
        material_id = std::stoi(parts[4]);
    } catch (...) {
        return {};
    }
    if (box_id < 0 || box_id > 64 || material_id < 0 || material_id > 16)
        return {};
    return nlohmann::json{{"id", parts[0]}, {"type", parts[1]}, {"color", parts[2]}, {"boxId", box_id}, {"materialId", material_id}};
}

// Auto-match rank used by the send dialog (X3 + Creality Print ordering):
// CFS type+colour > CFS type > ext type+colour > ext type, then colour among leftovers.
// Lower rank wins; colour_dist breaks ties. Returns -1 when nothing is usable.
inline int auto_match_index(const std::string &type, const std::string &colour, const std::vector<CfsSlot> &slots)
{
    auto rgb_dist = [](const std::string &a, const std::string &b) -> double {
        if (a.size() < 7 || b.size() < 7)
            return 1e9;
        auto hex = [](char c) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return c >= 'a' ? 10 + (c - 'a') : c - '0';
        };
        auto ch = [&](const std::string &s, int o) { return hex(s[o]) * 16 + hex(s[o + 1]); };
        const double dr = ch(a, 1) - ch(b, 1), dg = ch(a, 3) - ch(b, 3), db = ch(a, 5) - ch(b, 5);
        return dr * dr + dg * dg + db * db;
    };

    int    best   = -1;
    int    best_r = 99;
    double best_d = 1e12;
    for (size_t i = 0; i < slots.size(); ++i) {
        const CfsSlot &s = slots[i];
        if (!s.loaded)
            continue;
        const std::string plate_colour  = colour_hex(colour);
        const std::string slot_colour   = colour_hex(s.color_hex);
        const bool        type_ok       = !type.empty() && iequals_ascii(type, s.filament_type);
        const bool        colour_ok     = !plate_colour.empty() && !slot_colour.empty();
        const bool        type_conflict = !type.empty() && !s.filament_type.empty() && !iequals_ascii(type, s.filament_type);
        int               rank          = 99;
        if (type_ok && colour_ok && !s.external)
            rank = 0;
        else if (type_ok && !s.external)
            rank = 1;
        else if (type_ok && colour_ok && s.external)
            rank = 2;
        else if (type_ok && s.external)
            rank = 3;
        else if (colour_ok && !type_conflict)
            rank = 4;
        else
            continue;
        const double dist = colour_ok ? rgb_dist(plate_colour, slot_colour) : 1e9;
        if (best < 0 || rank < best_r || (rank == best_r && dist < best_d)) {
            best   = static_cast<int>(i);
            best_r = rank;
            best_d = dist;
        }
    }
    return best;
}

} // namespace CrealityPrintModel
} // namespace Slic3r

#endif

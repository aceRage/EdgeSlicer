#ifndef slic3r_CrealityPrintModel_hpp_
#define slic3r_CrealityPrintModel_hpp_

#include <map>
#include <string>
#include <unordered_set>

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

// K1-family: /usr/data. Everything else (K2 / K2 Pro / K2 Plus / SPARKX i7, and an
// unknown model) uses /mnt/UDISK, matching Creality Print and upstream Orca.
inline std::string gcode_root(const std::string &model) { return is_k1_family(model) ? "/usr/data" : "/mnt/UDISK"; }

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

} // namespace CrealityPrintModel
} // namespace Slic3r

#endif

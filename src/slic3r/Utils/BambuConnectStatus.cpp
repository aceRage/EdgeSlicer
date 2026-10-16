#include "BambuConnectStatus.hpp"

#include <cstdio>

#include <nlohmann/json.hpp>

#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace BambuConnectStatus {

using nlohmann::json;

const char *const STATUS_ONLY_SECTION = "bambu_status_only";
const char *const SNAPSHOT_SECTION    = "bambu_status_snapshot";

// -------------------------------------------------------------------------------------------
// Status-only printers
// -------------------------------------------------------------------------------------------

namespace {

// The single "<namespace>", "<command>" of a payload; false unless it has exactly one block with a
// string command.
bool single_command(const std::string &payload_json, std::string &ns, std::string &command)
{
    const json j = json::parse(payload_json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.size() != 1)
        return false;
    const auto block = j.begin();
    if (!block.value().is_object())
        return false;
    const auto cmd = block.value().find("command");
    if (cmd == block.value().end() || !cmd->is_string())
        return false;
    ns      = block.key();
    command = cmd->get<std::string>();
    return true;
}

} // namespace

bool status_only_allows(const std::string &payload_json)
{
    std::string ns, command;
    if (!single_command(payload_json, ns, command))
        return false;
    return (ns == "pushing" && command == "pushall") || (ns == "info" && command == "get_version");
}

std::string command_label(const std::string &payload_json)
{
    const json j = json::parse(payload_json, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return "?";
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_object())
            continue;
        const auto cmd = it.value().find("command");
        if (cmd != it.value().end() && cmd->is_string())
            return it.key() + "/" + cmd->get<std::string>();
    }
    return "?";
}

bool is_status_only(const AppConfig &config, const std::string &dev_id)
{
    return !dev_id.empty() && config.get(STATUS_ONLY_SECTION, dev_id) == "1";
}

void set_status_only(AppConfig &config, const std::string &dev_id, bool status_only)
{
    if (dev_id.empty())
        return;
    if (status_only)
        config.set(STATUS_ONLY_SECTION, dev_id, std::string("1"));
    else if (config.has(STATUS_ONLY_SECTION, dev_id))
        config.erase(STATUS_ONLY_SECTION, dev_id);
}

// -------------------------------------------------------------------------------------------
// Snapshot
// -------------------------------------------------------------------------------------------

namespace {

json option_to_json(const ConfigOption *opt)
{
    if (auto o = dynamic_cast<const ConfigOptionStrings *>(opt))
        return json{ { "t", "ss" }, { "v", o->values } };
    if (auto o = dynamic_cast<const ConfigOptionBools *>(opt)) {
        std::vector<bool> v;
        for (unsigned char b : o->values)
            v.push_back(b != 0);
        return json{ { "t", "bb" }, { "v", v } };
    }
    if (auto o = dynamic_cast<const ConfigOptionInts *>(opt))
        return json{ { "t", "ii" }, { "v", o->values } };
    if (auto o = dynamic_cast<const ConfigOptionFloats *>(opt))
        return json{ { "t", "ff" }, { "v", o->values } };
    if (auto o = dynamic_cast<const ConfigOptionString *>(opt))
        return json{ { "t", "s" }, { "v", o->value } };
    if (auto o = dynamic_cast<const ConfigOptionBool *>(opt))
        return json{ { "t", "b" }, { "v", o->value } };
    if (auto o = dynamic_cast<const ConfigOptionInt *>(opt))
        return json{ { "t", "i" }, { "v", o->value } };
    if (auto o = dynamic_cast<const ConfigOptionFloat *>(opt))
        return json{ { "t", "f" }, { "v", o->value } };
    return json();
}

ConfigOption *option_from_json(const json &j)
{
    if (!j.is_object() || !j.contains("t") || !j.contains("v") || !j["t"].is_string())
        return nullptr;
    const std::string t = j["t"].get<std::string>();
    const json       &v = j["v"];
    try {
        if (t == "ss" && v.is_array())
            return new ConfigOptionStrings(v.get<std::vector<std::string>>());
        if (t == "bb" && v.is_array()) {
            std::vector<unsigned char> out;
            for (const auto &b : v)
                out.push_back(b.is_boolean() && b.get<bool>() ? 1 : 0);
            auto *o   = new ConfigOptionBools();
            o->values = out;
            return o;
        }
        if (t == "ii" && v.is_array())
            return new ConfigOptionInts(v.get<std::vector<int>>());
        if (t == "ff" && v.is_array())
            return new ConfigOptionFloats(v.get<std::vector<double>>());
        if (t == "s" && v.is_string())
            return new ConfigOptionString(v.get<std::string>());
        if (t == "b" && v.is_boolean())
            return new ConfigOptionBool(v.get<bool>());
        if (t == "i" && v.is_number_integer())
            return new ConfigOptionInt(v.get<int>());
        if (t == "f" && v.is_number())
            return new ConfigOptionFloat(v.get<double>());
    } catch (...) {
    }
    return nullptr;
}

json content_json(const Snapshot &snap)
{
    json trays = json::array();
    for (const auto &kv : snap.trays) {
        json opts = json::object();
        for (const std::string &key : kv.second.keys()) {
            json o = option_to_json(kv.second.option(key));
            if (!o.is_null())
                opts[key] = o;
        }
        trays.push_back(json{ { "index", kv.first }, { "options", opts } });
    }
    json extruders = json::array();
    for (const ExtruderSnap &e : snap.extruders)
        extruders.push_back(json{ { "id", e.id }, { "nozzle", e.has_nozzle }, { "volume_type", e.volume_type }, { "diameter", e.diameter } });
    return json{ { "trays", trays }, { "extruders", extruders } };
}

} // namespace

std::string snapshot_to_json(const Snapshot &snap)
{
    json j          = content_json(snap);
    j["model"]      = snap.printer_model;
    j["name"]       = snap.printer_name;
    j["saved_at"]   = static_cast<long long>(snap.saved_at);
    return j.dump();
}

bool snapshot_from_json(const std::string &json_text, Snapshot &out)
{
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("trays") || !j["trays"].is_array())
        return false;
    Snapshot snap;
    try {
        if (j.contains("saved_at") && j["saved_at"].is_number_integer())
            snap.saved_at = static_cast<std::time_t>(j["saved_at"].get<long long>());
        if (j.contains("model") && j["model"].is_string())
            snap.printer_model = j["model"].get<std::string>();
        if (j.contains("name") && j["name"].is_string())
            snap.printer_name = j["name"].get<std::string>();
        for (const auto &t : j["trays"]) {
            if (!t.is_object() || !t.contains("index") || !t["index"].is_number_integer() || !t.contains("options") ||
                !t["options"].is_object())
                continue;
            DynamicPrintConfig cfg;
            for (auto it = t["options"].begin(); it != t["options"].end(); ++it)
                if (ConfigOption *opt = option_from_json(it.value()))
                    cfg.set_key_value(it.key(), opt);
            snap.trays.emplace(t["index"].get<int>(), std::move(cfg));
        }
        if (j.contains("extruders") && j["extruders"].is_array()) {
            for (const auto &e : j["extruders"]) {
                if (!e.is_object())
                    continue;
                ExtruderSnap x;
                x.id          = e.value("id", 0);
                x.has_nozzle  = e.value("nozzle", true);
                x.volume_type = e.value("volume_type", 0);
                x.diameter    = e.value("diameter", 0.4f);
                snap.extruders.push_back(x);
            }
        }
    } catch (...) {
        return false;
    }
    out = std::move(snap);
    return true;
}

bool same_content(const Snapshot &a, const Snapshot &b)
{
    return content_json(a) == content_json(b);
}

std::string pick_snapshot(const std::map<std::string, Snapshot> &all, const std::string &preferred_dev_id,
                          const std::string &preset_model)
{
    if (!preferred_dev_id.empty()) {
        auto it = all.find(preferred_dev_id);
        // Only for the printer model being sliced for (an unknown model on either side is let through).
        if (it != all.end() && !it->second.trays.empty() &&
            (preset_model.empty() || it->second.printer_model.empty() || it->second.printer_model == preset_model))
            return preferred_dev_id;
    }
    std::string best;
    std::time_t best_at = 0;
    for (const auto &kv : all) {
        if (kv.second.trays.empty() || preset_model.empty() || kv.second.printer_model != preset_model)
            continue;
        if (best.empty() || kv.second.saved_at > best_at) {
            best    = kv.first;
            best_at = kv.second.saved_at;
        }
    }
    return best;
}

std::string time_label(std::time_t saved_at, std::time_t now)
{
    std::tm a{}, b{};
#ifdef _WIN32
    localtime_s(&a, &saved_at);
    localtime_s(&b, &now);
#else
    localtime_r(&saved_at, &a);
    localtime_r(&now, &b);
#endif
    char buf[32];
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        std::snprintf(buf, sizeof(buf), "%02d:%02d", a.tm_hour, a.tm_min);
    else
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
    return buf;
}

std::vector<int> nozzle_volume_types_for(const std::vector<ExtruderSnap> &extruders, const std::vector<int> &physical_of_logical,
                                         std::vector<int> current)
{
    if (current.size() < physical_of_logical.size())
        current.resize(physical_of_logical.size(), 0);
    for (size_t logical = 0; logical < physical_of_logical.size(); ++logical)
        for (const ExtruderSnap &e : extruders)
            if (e.id == physical_of_logical[logical] && e.has_nozzle)
                current[logical] = e.volume_type;
    return current;
}

std::map<std::string, Snapshot> load_snapshots(const AppConfig &config)
{
    std::map<std::string, Snapshot> out;
    if (!config.has_section(SNAPSHOT_SECTION))
        return out;
    for (const auto &kv : config.get_section(SNAPSHOT_SECTION)) {
        Snapshot snap;
        if (snapshot_from_json(kv.second, snap))
            out.emplace(kv.first, std::move(snap));
    }
    return out;
}

bool store_snapshot(AppConfig &config, const std::string &dev_id, const Snapshot &snap, std::time_t refresh_after_s)
{
    if (dev_id.empty() || snap.trays.empty())
        return false;
    if (config.has(SNAPSHOT_SECTION, dev_id)) {
        Snapshot old;
        // Unchanged: only move "as of" forward now and then, not on every status push.
        if (snapshot_from_json(config.get(SNAPSHOT_SECTION, dev_id), old) && same_content(old, snap) &&
            old.printer_name == snap.printer_name && old.printer_model == snap.printer_model &&
            snap.saved_at - old.saved_at < refresh_after_s)
            return false;
    }
    config.set(SNAPSHOT_SECTION, dev_id, snapshot_to_json(snap));
    return true;
}

} // namespace BambuConnectStatus
} // namespace Slic3r

#include "PrintHostDevices.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>

namespace Slic3r {
namespace PrintHostDevices {

using nlohmann::json;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------- the file ----

static std::mutex  s_mutex;
static std::string s_store_path_override;

std::string store_path()
{
    if (!s_store_path_override.empty())
        return s_store_path_override;
    return (fs::path(data_dir()) / "hub" / "print_host_devices.json").string();
}

void set_store_path(const std::string& path) { s_store_path_override = path; }

// Anything unreadable is an empty store: a printer list is never worth refusing to start over.
static json load_store()
{
    try {
        boost::nowide::ifstream in(store_path());
        if (!in.good())
            return json::object();
        json j;
        in >> j;
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

// Through a temporary file, as SnapmakerLan does: another instance reading the list never sees
// half of it.
static void save_store(const json& j)
{
    try {
        boost::system::error_code ec;
        const fs::path            final(store_path());
        if (final.has_parent_path())
            fs::create_directories(final.parent_path(), ec);
        const fs::path temp = final.parent_path() / (final.filename().string() + ".tmp");
        {
            boost::nowide::ofstream out(temp.string(), std::ios::binary | std::ios::trunc);
            out << j.dump(1);
        }
        fs::remove(final, ec);
        fs::rename(temp, final, ec);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[PrintHostDevices] could not write the device list: " << e.what();
    }
}

static long long now_s()
{
    return (long long) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string str_of(const json& j, const char* key, const std::string& def = std::string())
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : def;
}

static long long num_of(const json& j, const char* key, long long def = 0)
{
    return j.is_object() && j.contains(key) && j[key].is_number_integer() ? j[key].get<long long>() : def;
}

// ---------------------------------------------------------------- the key ----

static std::string trimmed(const std::string& s)
{
    std::string out = s;
    boost::trim(out);
    return out;
}

static std::string lowered(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return out;
}

// A string option of a config that may simply not have it (a bare DynamicPrintConfig, a config
// built from a partial preset): "" rather than a crash.
static std::string cfg_str(const DynamicPrintConfig& config, const char* key)
{
    const ConfigOptionString* opt = config.option<ConfigOptionString>(key);
    return opt ? opt->value : std::string();
}

std::string model_key(const std::string& printer_model, const std::string& preset_name)
{
    const std::string model = trimmed(printer_model);
    if (!model.empty())
        return model;
    // A preset with no printer_model at all (hand-made, or a "Custom Printer") is its own model:
    // grouping every such preset together would put unrelated machines in one list.
    return "preset:" + trimmed(preset_name);
}

std::string model_key_for(const Preset& printer_preset)
{
    return model_key(cfg_str(printer_preset.config, "printer_model"), printer_preset.name);
}

std::string current_model_key(const PresetBundle& bundle)
{
    return model_key_for(bundle.printers.get_edited_preset());
}

std::string normalize_address(const std::string& address)
{
    std::string a = lowered(trimmed(address));
    while (!a.empty() && (a.back() == '/' || a.back() == ' '))
        a.pop_back();
    return a;
}

// ---------------------------------------------------------- device <-> json ----

static Device device_of(const json& j)
{
    Device d;
    d.id            = str_of(j, "id");
    d.alias         = str_of(j, "alias");
    d.address       = str_of(j, "address");
    d.host_type     = str_of(j, "host_type", "octoprint");
    d.auth_type     = str_of(j, "auth_type", "key");
    d.apikey        = str_of(j, "apikey");
    d.user          = str_of(j, "user");
    d.password      = str_of(j, "password");
    d.printer_model = str_of(j, "printer_model");
    d.created       = num_of(j, "created");
    d.last_used     = num_of(j, "last_used");
    return d;
}

static json json_of(const Device& d)
{
    json j;
    j["id"]            = d.id;
    j["alias"]         = d.alias;
    j["address"]       = d.address;
    j["host_type"]     = d.host_type;
    j["auth_type"]     = d.auth_type;
    j["apikey"]        = d.apikey;
    j["user"]          = d.user;
    j["password"]      = d.password;
    j["printer_model"] = d.printer_model;
    j["created"]       = d.created;
    j["last_used"]     = d.last_used;
    return j;
}

// The store is { "version": 1, "models": { "<key>": { devices, current, migrated } } }.
static json& model_node(json& store, const std::string& key)
{
    if (!store.contains("version") || !store["version"].is_number_integer())
        store["version"] = 1;
    if (!store.contains("models") || !store["models"].is_object())
        store["models"] = json::object();
    json& models = store["models"];
    if (!models.contains(key) || !models[key].is_object())
        models[key] = json::object();
    json& node = models[key];
    if (!node.contains("devices") || !node["devices"].is_array())
        node["devices"] = json::array();
    return node;
}

static const json empty_node = json::object();

static const json& model_node_ro(const json& store, const std::string& key)
{
    if (!store.contains("models") || !store["models"].is_object())
        return empty_node;
    const json& models = store["models"];
    auto        it     = models.find(key);
    if (it == models.end() || !it->is_object())
        return empty_node;
    return *it;
}

static std::vector<Device> devices_of(const json& node)
{
    std::vector<Device> out;
    if (!node.contains("devices") || !node["devices"].is_array())
        return out;
    for (const json& e : node["devices"])
        if (e.is_object() && !str_of(e, "address").empty())
            out.push_back(device_of(e));
    return out;
}

// An id nobody else in this model has. Seeded from the model and the address so the same
// migration on two machines produces the same id; a collision walks a counter.
static std::string new_id(const std::string& key, const std::string& address, const std::vector<Device>& taken)
{
    const std::string seed = key + "|" + normalize_address(address);
    for (int n = 0; n < 1000; ++n) {
        const std::size_t h = std::hash<std::string>{}(n == 0 ? seed : seed + "|" + std::to_string(n));
        char              buf[32];
        std::snprintf(buf, sizeof buf, "d%012llx", (unsigned long long) (h & 0xFFFFFFFFFFFFull));
        const std::string id(buf);
        bool              clash = false;
        for (const Device& d : taken)
            if (d.id == id) {
                clash = true;
                break;
            }
        if (!clash)
            return id;
    }
    return "d" + std::to_string(now_s());
}

// ------------------------------------------------------------- the list ----

std::vector<Device> devices(const std::string& key)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return devices_of(model_node_ro(load_store(), key));
}

std::map<std::string, std::vector<Device>> all_devices()
{
    std::lock_guard<std::mutex>                lock(s_mutex);
    std::map<std::string, std::vector<Device>> out;
    const json                                 store = load_store();
    if (!store.contains("models") || !store["models"].is_object())
        return out;
    for (auto it = store["models"].begin(); it != store["models"].end(); ++it) {
        std::vector<Device> list = devices_of(*it);
        if (!list.empty())
            out[it.key()] = std::move(list);
    }
    return out;
}

bool find(const std::string& key, const std::string& id, Device& out)
{
    for (const Device& d : devices(key))
        if (d.id == id) {
            out = d;
            return true;
        }
    return false;
}

bool add(const std::string& key, Device& d, std::string& error)
{
    if (trimmed(d.address).empty()) {
        error = "the address is empty";
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    json                        store = load_store();
    json&                       node  = model_node(store, key);
    const std::vector<Device>   list  = devices_of(node);
    for (const Device& e : list)
        if (normalize_address(e.address) == normalize_address(d.address)) {
            error = "that address is already in this printer's list";
            return false;
        }
    d.address = trimmed(d.address);
    if (d.id.empty())
        d.id = new_id(key, d.address, list);
    if (d.created == 0)
        d.created = now_s();
    if (d.host_type.empty())
        d.host_type = "octoprint";
    if (d.auth_type.empty())
        d.auth_type = "key";
    node["devices"].push_back(json_of(d));
    save_store(store);
    return true;
}

bool update(const std::string& key, const Device& d, std::string& error)
{
    if (trimmed(d.address).empty()) {
        error = "the address is empty";
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    json                        store = load_store();
    json&                       node  = model_node(store, key);
    for (const json& e : node["devices"])
        if (str_of(e, "id") != d.id && normalize_address(str_of(e, "address")) == normalize_address(d.address)) {
            error = "that address is already in this printer's list";
            return false;
        }
    for (json& e : node["devices"])
        if (str_of(e, "id") == d.id) {
            Device merged  = d;
            merged.address = trimmed(merged.address);
            merged.created = num_of(e, "created", merged.created);
            e              = json_of(merged);
            save_store(store);
            return true;
        }
    error = "no such device";
    return false;
}

bool remove(const std::string& key, const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    json                        store = load_store();
    json&                       node  = model_node(store, key);
    json                        kept  = json::array();
    bool                        found = false;
    for (const json& e : node["devices"]) {
        if (str_of(e, "id") == id) {
            found = true;
            continue;
        }
        kept.push_back(e);
    }
    if (!found)
        return false;
    node["devices"] = kept;
    if (str_of(node, "last_used") == id)
        node["last_used"] = "";
    if (str_of(node, "current") == id)
        node["current"] = "";
    save_store(store);
    return true;
}

std::string last_used_id(const std::string& key)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    // The store is kept in a local: model_node_ro returns a reference *into* it, and binding that
    // to the temporary load_store() returns leaves it dangling as soon as the statement ends.
    const json                  store = load_store();
    const json&                 node  = model_node_ro(store, key);
    const std::string           id    = str_of(node, "last_used");
    // A store written by phase 1 has "current" instead - the device its "Use this device" button
    // had written into the preset. It is the best guess at "the one you last sent to", so it is
    // read once and then quietly replaced the next time a send happens.
    return id.empty() ? str_of(node, "current") : id;
}

void set_last_used(const std::string& key, const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    json                        store = load_store();
    json&                       node  = model_node(store, key);
    node["last_used"]                 = id;
    // phase 1's key, so an older build reading this file does not resurrect a stale choice.
    if (node.contains("current"))
        node["current"] = id;
    for (json& e : node["devices"])
        if (str_of(e, "id") == id)
            e["last_used"] = now_s();
    save_store(store);
}

// ------------------------------------------------------- the preset bridge ----

PrintHostType host_type_enum(const std::string& key)
{
    const t_config_enum_values& values = ConfigOptionEnum<PrintHostType>::get_enum_values();
    auto                        it     = values.find(key);
    return it == values.end() ? htOctoPrint : (PrintHostType) it->second;
}

std::string host_type_key(PrintHostType type)
{
    for (const auto& kv : ConfigOptionEnum<PrintHostType>::get_enum_values())
        if (kv.second == (int) type)
            return kv.first;
    return "octoprint";
}

bool speaks_moonraker(const std::string& key)
{
    // "octoprint" is what this fork labels "Octo/Klipper" (PrintConfig.cpp:4682) and is the host
    // type a Moonraker box is configured as; the two htMoonRaker* enum members have no key of their
    // own (they are set by the Device tab's connect, not by the preset), so they are named here for
    // completeness only.
    return key == "octoprint" || key == "moonraker" || key == "moonraker_mqtt";
}

Device from_config(const DynamicPrintConfig& config)
{
    Device d;
    d.address       = cfg_str(config, "print_host");
    d.apikey        = cfg_str(config, "printhost_apikey");
    d.user          = cfg_str(config, "printhost_user");
    d.password      = cfg_str(config, "printhost_password");
    d.printer_model = cfg_str(config, "printer_model");
    if (const auto* opt = config.option<ConfigOptionEnum<PrintHostType>>("host_type"))
        d.host_type = host_type_key(opt->value);
    else
        d.host_type = "octoprint";
    if (const auto* opt = config.option<ConfigOptionEnum<AuthorizationType>>("printhost_authorization_type"))
        d.auth_type = opt->value == atUserPassword ? "user" : "key";
    else
        d.auth_type = "key";
    return d;
}

DynamicPrintConfig config_for(const Device& d, const DynamicPrintConfig& preset_config)
{
    DynamicPrintConfig cfg = preset_config;
    apply_to_config(d, cfg);
    return cfg;
}

void apply_to_config(const Device& d, DynamicPrintConfig& config)
{
    // create = true: a config that does not carry the host options yet gets them, so this works on a
    // bare DynamicPrintConfig as well as on a printer preset's.
    config.opt_string("print_host", true)         = d.address;
    config.opt_string("printhost_apikey", true)   = d.apikey;
    config.opt_string("printhost_user", true)     = d.user;
    config.opt_string("printhost_password", true) = d.password;
    if (auto* opt = config.option<ConfigOptionEnum<PrintHostType>>("host_type", true))
        opt->value = host_type_enum(d.host_type);
    if (auto* opt = config.option<ConfigOptionEnum<AuthorizationType>>("printhost_authorization_type", true))
        opt->value = d.auth_type == "user" ? atUserPassword : atKeyPassword;
}

// --------------------------------------------------------------- migration ----

int migrate_from_presets(const std::vector<PresetHost>& presets)
{
    int created = 0;
    for (const PresetHost& p : presets) {
        const std::string address = trimmed(p.address);
        if (address.empty() || p.model_key.empty())
            continue;
        const std::string norm = normalize_address(address);
        std::lock_guard<std::mutex> lock(s_mutex);
        json                        store = load_store();
        json&                       node  = model_node(store, p.model_key);
        if (!node.contains("migrated") || !node["migrated"].is_array())
            node["migrated"] = json::array();
        // Already imported once: a device somebody deleted afterwards stays deleted.
        bool already = false;
        for (const json& m : node["migrated"])
            if (m.is_string() && m.get<std::string>() == norm) {
                already = true;
                break;
            }
        if (already)
            continue;
        node["migrated"].push_back(norm);
        bool have = false;
        for (const json& e : node["devices"])
            if (normalize_address(str_of(e, "address")) == norm) {
                have = true;
                break;
            }
        if (!have) {
            Device d;
            d.id            = new_id(p.model_key, address, devices_of(node));
            d.alias         = p.preset_name.empty() ? address : p.preset_name;
            d.address       = address;
            d.host_type     = p.host_type.empty() ? "octoprint" : p.host_type;
            d.auth_type     = p.auth_type.empty() ? "key" : p.auth_type;
            d.apikey        = p.apikey;
            d.user          = p.user;
            d.password      = p.password;
            d.printer_model = p.printer_model;
            d.created       = now_s();
            node["devices"].push_back(json_of(d));
            // Deliberately not marked as the model's device: an import is not a send, and this
            // feature has no "main printer" to be. The send dialog preselects the first row when
            // nothing was ever sent.
            ++created;
        }
        save_store(store);
    }
    return created;
}

int migrate_from_presets(const PresetBundle& bundle)
{
    std::vector<PresetHost> list;
    const PresetCollection& printers = bundle.printers;
    for (auto it = printers.begin(); it != printers.end(); ++it) {
        const Preset& preset = *it;
        if (!preset.is_visible)
            continue;
        const std::string address = cfg_str(preset.config, "print_host");
        if (trimmed(address).empty())
            continue;
        const Device d = from_config(preset.config);
        PresetHost   p;
        p.model_key     = model_key_for(preset);
        p.preset_name   = preset.name;
        p.address       = d.address;
        p.host_type     = d.host_type;
        p.auth_type     = d.auth_type;
        p.apikey        = d.apikey;
        p.user          = d.user;
        p.password      = d.password;
        p.printer_model = d.printer_model;
        list.push_back(std::move(p));
    }
    // The preset being edited right now may hold an address that was never saved to disk.
    const Preset& edited = printers.get_edited_preset();
    if (!trimmed(cfg_str(edited.config, "print_host")).empty()) {
        const Device d = from_config(edited.config);
        PresetHost   p;
        p.model_key     = model_key_for(edited);
        p.preset_name   = edited.name;
        p.address       = d.address;
        p.host_type     = d.host_type;
        p.auth_type     = d.auth_type;
        p.apikey        = d.apikey;
        p.user          = d.user;
        p.password      = d.password;
        p.printer_model = d.printer_model;
        list.push_back(std::move(p));
    }
    return migrate_from_presets(list);
}

} // namespace PrintHostDevices
} // namespace Slic3r

#include "PluginResolver.hpp"

#include "PluginManager.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
namespace {

std::vector<MissingPlugin> g_missing_cloud;
std::vector<MissingPlugin> g_missing_local;
std::vector<MissingPlugin> g_inactive;
std::vector<MissingPlugin> g_broken;

bool is_tracked_type(Preset::Type type)
{
    return type == Preset::TYPE_PRINT || type == Preset::TYPE_PRINTER || type == Preset::TYPE_FILAMENT;
}

void erase_type(std::vector<MissingPlugin>& list, Preset::Type type)
{
    list.erase(std::remove_if(list.begin(), list.end(), [type](const MissingPlugin& m) { return m.opt_type == type; }),
               list.end());
}

std::set<std::string> option_referenced_capability_names(const Preset& preset)
{
    std::set<std::string> used;
    const ConfigDef* def = preset.config.def();
    for (const std::string& opt_key : preset.config.keys()) {
        const ConfigOptionDef* opt_def = def != nullptr ? def->get(opt_key) : nullptr;
        if (opt_def == nullptr || !opt_def->is_plugin_backed())
            continue;
        const ConfigOption* opt = preset.config.option(opt_key);
        if (const auto* scalar = dynamic_cast<const ConfigOptionString*>(opt)) {
            if (!scalar->value.empty())
                used.insert(scalar->value);
        } else if (const auto* vector = dynamic_cast<const ConfigOptionVectorBase*>(opt)) {
            for (const std::string& value : vector->vserialize())
                if (!value.empty())
                    used.insert(value);
        }
    }
    return used;
}

std::string find_option_for_capability(const Preset& preset, const std::string& capability_name)
{
    const ConfigDef* def = preset.config.def();
    for (const std::string& opt_key : preset.config.keys()) {
        const ConfigOptionDef* opt_def = def != nullptr ? def->get(opt_key) : nullptr;
        if (opt_def == nullptr || !opt_def->is_plugin_backed())
            continue;
        const ConfigOption* opt = preset.config.option(opt_key);
        if (const auto* scalar = dynamic_cast<const ConfigOptionString*>(opt)) {
            if (scalar->value == capability_name)
                return opt_key;
        } else if (const auto* vector = dynamic_cast<const ConfigOptionVectorBase*>(opt)) {
            for (const std::string& value : vector->vserialize())
                if (value == capability_name)
                    return opt_key;
        }
    }
    return {};
}

PluginCapabilityType type_for_option(const Preset& preset, const std::string& opt_key)
{
    const ConfigDef* def = preset.config.def();
    const ConfigOptionDef* opt_def = def != nullptr ? def->get(opt_key) : nullptr;
    if (opt_def == nullptr || opt_def->plugin_type.empty())
        return PluginCapabilityType::Unknown;
    return plugin_capability_type_from_string(opt_def->plugin_type);
}

} // namespace

void refresh_missing_plugins(Preset::Type type, const ConfigOptionStrings* manifest, const Preset* preset)
{
    erase_type(g_missing_cloud, type);
    erase_type(g_missing_local, type);
    erase_type(g_inactive, type);
    erase_type(g_broken, type);

    if (!is_tracked_type(type) || manifest == nullptr || preset == nullptr)
        return;

    PluginManager& mgr = PluginManager::instance();
    for (const PluginCapabilityRef& ref : referenced_capabilities(type, *preset)) {
        const std::string opt_key    = find_option_for_capability(*preset, ref.capability_name);
        const PluginCapabilityType cap_type = type_for_option(*preset, opt_key);
        MissingPlugin mp;
        mp.ref          = ref;
        mp.opt          = opt_key;
        mp.opt_type     = type;
        mp.type         = cap_type;
        mp.recovery_url = {};

        const std::string plugin_key = ref.uuid.empty() ? ref.name : ref.uuid;
        PluginDescriptor descriptor;
        if (!mgr.try_get_plugin_descriptor(plugin_key, descriptor)) {
            if (ref.uuid.empty())
                g_missing_local.push_back(std::move(mp));
            else
                g_missing_cloud.push_back(std::move(mp));
            continue;
        }

        auto cap = mgr.get_plugin_capability({cap_type, ref.capability_name, plugin_key}, /*only_enabled=*/false);
        if (!cap) {
            if (mgr.is_plugin_loaded(plugin_key))
                g_broken.push_back(std::move(mp));
            else
                g_inactive.push_back(std::move(mp));
            continue;
        }
        if (!cap->is_enabled())
            g_inactive.push_back(std::move(mp));
    }
}

void refresh_missing_plugins(const PresetBundle& preset_bundle)
{
    refresh_missing_plugins(Preset::TYPE_PRINT, preset_bundle.prints.get_edited_preset().config.option<ConfigOptionStrings>("plugins"),
                            &preset_bundle.prints.get_edited_preset());
    refresh_missing_plugins(Preset::TYPE_PRINTER, preset_bundle.printers.get_edited_preset().config.option<ConfigOptionStrings>("plugins"),
                            &preset_bundle.printers.get_edited_preset());
    refresh_missing_plugins(Preset::TYPE_FILAMENT, preset_bundle.filaments.get_edited_preset().config.option<ConfigOptionStrings>("plugins"),
                            &preset_bundle.filaments.get_edited_preset());
}

std::vector<MissingPlugin> get_missing_cloud_plugins() { return g_missing_cloud; }
std::vector<MissingPlugin> get_missing_local_plugins() { return g_missing_local; }
bool                     has_missing_plugins() { return !g_missing_cloud.empty() || !g_missing_local.empty(); }

std::vector<MissingPlugin> get_inactive_plugins() { return g_inactive; }
bool                     has_inactive_plugins() { return !g_inactive.empty(); }

std::vector<MissingPlugin> get_broken_plugins() { return g_broken; }
bool                     has_broken_plugins() { return !g_broken.empty(); }

void resolve_missing_plugins(const std::vector<std::string>& /*refs*/, PluginInstallProgress /*progress*/)
{
    // Edge has no Orca Cloud catalog. Missing plugins stay missing until the user installs a local file.
}

void resolve_inactive_plugins(const std::vector<std::string>& refs)
{
    PluginManager& mgr = PluginManager::instance();
    for (const std::string& entry : refs) {
        auto parsed = parse_capability_ref(entry);
        if (!parsed)
            continue;
        const std::string plugin_key = parsed->uuid.empty() ? parsed->name : parsed->uuid;
        if (!mgr.is_plugin_loaded(plugin_key))
            mgr.load_plugin(plugin_key, /*skip_deps=*/false, {});
        std::string error;
        mgr.wait_for_plugin_load(plugin_key, std::chrono::seconds(30), error);
        auto cap = mgr.get_plugin_capability(parsed->capability_name, PluginCapabilityType::Unknown, /*only_enabled=*/false);
        if (cap && !cap->is_enabled())
            mgr.set_capability_enabled({cap->type(), cap->name(), plugin_key}, true);
    }
}

void open_missing_plugins_on_cloud(const std::vector<std::string>& /*local_refs*/)
{
    // No catalog in this phase.
}

std::string create_full_ref(const PluginCapabilityRef& ref)
{
    return ref.name + ';' + ref.uuid + ';' + ref.capability_name;
}

std::string resolve_recovery_url(const PluginCapabilityRef& /*ref*/) { return {}; }

std::vector<PluginCapabilityRef> referenced_capabilities(Preset::Type type, const Preset& preset)
{
    if (!is_tracked_type(type))
        return {};

    const auto* manifest = preset.config.option<ConfigOptionStrings>("plugins");
    if (manifest == nullptr)
        return {};

    const std::set<std::string> used = option_referenced_capability_names(preset);
    std::vector<PluginCapabilityRef> out;
    for (const std::string& entry : manifest->values) {
        auto parsed = parse_capability_ref(entry);
        if (!parsed)
            continue;
        if (used.count(parsed->capability_name))
            out.push_back(std::move(*parsed));
    }
    return out;
}

std::vector<PluginCapabilityId> capabilities_in_use(Preset::Type type, const Preset& preset)
{
    std::vector<PluginCapabilityId> out;
    for (const PluginCapabilityRef& ref : referenced_capabilities(type, preset)) {
        const std::string opt_key = find_option_for_capability(preset, ref.capability_name);
        out.push_back({type_for_option(preset, opt_key), ref.capability_name, ref.uuid.empty() ? ref.name : ref.uuid});
    }
    return out;
}

std::vector<PluginCapabilityId> capabilities_in_use(const PresetBundle& preset_bundle, Preset::Type type)
{
    const PresetCollection* collection = nullptr;
    switch (type) {
    case Preset::TYPE_PRINT:    collection = &preset_bundle.prints; break;
    case Preset::TYPE_PRINTER:  collection = &preset_bundle.printers; break;
    case Preset::TYPE_FILAMENT: collection = &preset_bundle.filaments; break;
    default: return {};
    }
    return capabilities_in_use(type, collection->get_edited_preset());
}

bool check_capability_in_use(const std::string& capability_refs)
{
    auto parsed = parse_capability_ref(capability_refs);
    if (!parsed)
        return false;
    for (const MissingPlugin& m : g_missing_local)
        if (m.ref.capability_name == parsed->capability_name && m.ref.name == parsed->name)
            return true;
    for (const MissingPlugin& m : g_missing_cloud)
        if (m.ref.capability_name == parsed->capability_name && m.ref.name == parsed->name)
            return true;
    for (const MissingPlugin& m : g_inactive)
        if (m.ref.capability_name == parsed->capability_name && m.ref.name == parsed->name)
            return true;
    for (const MissingPlugin& m : g_broken)
        if (m.ref.capability_name == parsed->capability_name && m.ref.name == parsed->name)
            return true;
    return false;
}

std::string missing_plugins_warning_message()
{
    std::vector<std::string> names;
    auto collect = [&names](const std::vector<MissingPlugin>& list) {
        for (const MissingPlugin& m : list) {
            const std::string label = m.ref.name + " / " + m.ref.capability_name;
            if (std::find(names.begin(), names.end(), label) == names.end())
                names.push_back(label);
        }
    };
    collect(g_missing_local);
    collect(g_missing_cloud);
    collect(g_broken);
    if (names.empty())
        return {};
    std::string text = "This preset or project references plugins that are not available:\n";
    for (const std::string& n : names)
        text += "  - " + n + "\n";
    text += "Install the missing plugin, or the slice will skip it with a warning.";
    return text;
}

std::string unresolved_plugins_save_message(const std::vector<ConfigBase::UnresolvedPluginCapability>& unresolved)
{
    if (unresolved.empty())
        return {};
    std::string text = "These plugin capabilities are selected but not currently available:\n";
    for (const auto& u : unresolved)
        text += "  - " + u.capability_name + " (" + u.opt_key + ")\n";
    text += "Save anyway? The reference will be omitted from the plugins manifest until the plugin is installed.";
    return text;
}

} // namespace Slic3r

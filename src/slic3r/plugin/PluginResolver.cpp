#include "PluginResolver.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace {

std::vector<MissingPlugin> g_missing_cloud;
std::vector<MissingPlugin> g_missing_local;
std::vector<MissingPlugin> g_inactive;
std::vector<MissingPlugin> g_broken;

} // namespace

void refresh_missing_plugins(Preset::Type /*type*/, const ConfigOptionStrings* /*manifest*/, const Preset* /*preset*/)
{
    // Phase 1: no plugin-backed PrintConfig options exist, so there is nothing to refresh.
}

void refresh_missing_plugins(const PresetBundle& /*preset_bundle*/) {}

std::vector<MissingPlugin> get_missing_cloud_plugins() { return g_missing_cloud; }
std::vector<MissingPlugin> get_missing_local_plugins() { return g_missing_local; }
bool                     has_missing_plugins() { return false; }

std::vector<MissingPlugin> get_inactive_plugins() { return g_inactive; }
bool                     has_inactive_plugins() { return false; }

std::vector<MissingPlugin> get_broken_plugins() { return g_broken; }
bool                     has_broken_plugins() { return false; }

void resolve_missing_plugins(const std::vector<std::string>& /*refs*/, PluginInstallProgress /*progress*/)
{
    // Phase 1: Orca Cloud install is not wired.
}

void resolve_inactive_plugins(const std::vector<std::string>& /*refs*/) {}

void open_missing_plugins_on_cloud(const std::vector<std::string>& /*local_refs*/) {}

std::string create_full_ref(const PluginCapabilityRef& ref)
{
    return ref.name + ';' + ref.uuid + ';' + ref.capability_name;
}

std::string resolve_recovery_url(const PluginCapabilityRef& /*ref*/) { return {}; }

std::vector<PluginCapabilityRef> referenced_capabilities(Preset::Type /*type*/, const Preset& /*preset*/)
{
    return {};
}

std::vector<PluginCapabilityId> capabilities_in_use(Preset::Type /*type*/, const Preset& /*preset*/)
{
    return {};
}

std::vector<PluginCapabilityId> capabilities_in_use(const PresetBundle& /*preset_bundle*/, Preset::Type /*type*/)
{
    return {};
}

bool check_capability_in_use(const std::string& /*capability_refs*/) { return false; }

} // namespace Slic3r

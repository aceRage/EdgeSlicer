#ifndef slic3r_GUI_PluginsInspectorLogic_hpp_
#define slic3r_GUI_PluginsInspectorLogic_hpp_

#include <string>

#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>

namespace Slic3r {

struct PluginDescriptor;

namespace GUI {

nlohmann::json build_permissions_payload(const PluginPermissions& permissions);

// One plugin as the inspector page sees it: display fields, stored grants, and configurable
// capabilities. Never includes plugin_root or entry_path.
nlohmann::json build_inspector_plugin_payload(const PluginDescriptor& descriptor);

struct PermissionMutationParse
{
    bool                     ok = false;
    bool                     needs_full_access_confirm = false;
    PluginPermissionMutation mutation;
    std::string              plugin_key;
    std::string              request_id;
    wxString                 error;
};

// Validates a page `mutate_plugin_permission` payload against the stored sidecar and the same
// grant rules as the editor. Does not write. A filesystem add that would cover UltraNet's
// {data_dir}/plugins is refused.
PermissionMutationParse parse_permission_mutation(const nlohmann::json& payload, const PluginPermissions& stored);

// True when `command` is a Home > Plugins action that opens the inspector.
bool is_inspector_home_command(const std::string& command);

} // namespace GUI
} // namespace Slic3r

#endif

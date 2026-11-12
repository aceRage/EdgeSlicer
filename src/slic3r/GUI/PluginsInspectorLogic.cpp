#include "PluginsInspectorLogic.hpp"

#include "I18N.hpp"
#include "PluginPermissionValidation.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginConfig.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

namespace Slic3r { namespace GUI {
namespace {

std::string string_field(const nlohmann::json& payload, const char* key)
{
    return payload.contains(key) && payload.at(key).is_string() ? payload.at(key).get<std::string>() : std::string{};
}

bool is_valid_bridge_request_id(const std::string& request_id)
{
    return !request_id.empty() && request_id.size() < 128;
}

PluginPermissionCategory category_from_name(const std::string& category, const std::vector<std::string>*& entries,
                                            const PluginPermissions& stored)
{
    if (category == "fs_read") {
        entries = &stored.fs_read;
        return PluginPermissionCategory::FsRead;
    }
    if (category == "fs_readwrite") {
        entries = &stored.fs_readwrite;
        return PluginPermissionCategory::FsReadWrite;
    }
    if (category == "network_http") {
        entries = &stored.network_http;
        return PluginPermissionCategory::NetworkHttp;
    }
    if (category == "network_socket") {
        entries = &stored.network_socket;
        return PluginPermissionCategory::NetworkSocket;
    }
    if (category == "process") {
        entries = &stored.process;
        return PluginPermissionCategory::Process;
    }
    if (category == "events") {
        entries = &stored.events;
        return PluginPermissionCategory::Events;
    }
    entries = nullptr;
    return PluginPermissionCategory::FsRead;
}

bool filesystem_grant_is_ultranet(const std::string& value)
{
    return PluginAuditManager::instance().is_ultranet_plugins_path(boost::filesystem::path(value));
}

} // namespace

nlohmann::json build_permissions_payload(const PluginPermissions& permissions)
{
    nlohmann::json payload;
    payload["fs_read"]        = permissions.fs_read;
    payload["fs_readwrite"]   = permissions.fs_readwrite;
    payload["network_http"]   = permissions.network_http;
    payload["network_socket"] = permissions.network_socket;
    payload["process"]        = permissions.process;
    payload["threading"]      = permissions.threading;
    payload["events"]         = permissions.events;
    payload["full_access"]    = permissions.full_access;
    return payload;
}

nlohmann::json build_inspector_plugin_payload(const PluginDescriptor& descriptor)
{
    nlohmann::json payload;
    payload["plugin_key"]  = descriptor.plugin_key;
    payload["name"]        = descriptor.name.empty() ? descriptor.plugin_key : descriptor.name;
    payload["description"] = descriptor.description;
    payload["author"]      = descriptor.author;
    payload["version"]     = descriptor.installed_version.empty() ? descriptor.version : descriptor.installed_version;
    payload["types"]       = descriptor.display_types;
    payload["installed"]   = descriptor.has_local_package();

    PluginInstallState state;
    if (PluginManager::instance().get_install_state(descriptor.plugin_key, state))
        payload["permissions"] = build_permissions_payload(state.permissions);
    else
        payload["permissions"] = build_permissions_payload(PluginPermissions{});

    std::vector<PluginCapabilityId> ids;
    for (const auto& cap : PluginManager::instance().get_plugin_capabilities(descriptor.plugin_key,
                                                                             PluginCapabilityType::Unknown, false)) {
        if (!cap)
            continue;
        ids.push_back(cap->identity());
    }
    payload["config_capabilities"] = PluginConfig::capabilities_payload(ids);
    payload["capabilities"]        = payload["config_capabilities"];
    return payload;
}

PermissionMutationParse parse_permission_mutation(const nlohmann::json& payload, const PluginPermissions& stored)
{
    PermissionMutationParse result;
    result.plugin_key = string_field(payload, "plugin_key");
    result.request_id = string_field(payload, "request_id");
    const std::string operation = string_field(payload, "operation");
    const std::string category  = string_field(payload, "category");
    if (result.plugin_key.empty() || !is_valid_bridge_request_id(result.request_id)) {
        result.error = _L("Invalid permission save request.");
        return result;
    }

    const std::vector<std::string>* entries = nullptr;
    const PluginPermissionCategory  bucket  = category_from_name(category, entries, stored);

    if (category == "full_access" || category == "threading") {
        if (operation != "set" || !payload.contains("value") || !payload.at("value").is_boolean()) {
            result.error = _L("Invalid permission save request.");
            return result;
        }
        const bool enabled = payload.at("value").get<bool>();
        if (category == "full_access") {
            result.needs_full_access_confirm = enabled;
            result.mutation.full_access      = enabled;
        } else {
            result.mutation.threading = enabled;
        }
        result.ok = true;
        return result;
    }

    if (entries == nullptr || (operation != "add" && operation != "remove" && operation != "replace") ||
        (operation != "add" && (!payload.contains("old_value") || !payload.at("old_value").is_string())) ||
        (operation != "remove" && (!payload.contains("value") || !payload.at("value").is_string()))) {
        result.error = _L("Invalid permission save request.");
        return result;
    }
    if (operation != "add") {
        const std::string old_value = string_field(payload, "old_value");
        if (std::find(entries->begin(), entries->end(), old_value) == entries->end()) {
            result.error = _L("Invalid permission save request.");
            return result;
        }
        result.mutation.remove.push_back({bucket, old_value});
    }
    if (operation != "remove") {
        const std::string raw   = string_field(payload, "value");
        const std::string value = trim_permission_target(raw);
        std::string       normalized = value;
        wxString          reason;
        bool              valid = !value.empty() && value.find('\0') == std::string::npos;
        if (valid && (category == "fs_read" || category == "fs_readwrite"))
            valid = normalize_filesystem_permission(value, normalized, reason);
        else if (valid && category == "network_http")
            valid = normalize_network_permission(value, normalized, reason);
        else if (valid && category == "network_socket")
            valid = normalize_socket_permission(value, normalized, reason);
        if (valid && (category == "fs_read" || category == "fs_readwrite") && filesystem_grant_is_ultranet(normalized)) {
            valid  = false;
            reason = _L("Python plugins cannot be granted access to EdgeSlicer's network plug-in folder.");
        }
        if (!valid) {
            result.error = reason.empty() ? _L("Invalid permission save request.") : reason;
            return result;
        }
        result.mutation.add.push_back({bucket, normalized});
    }
    result.ok = true;
    return result;
}

bool is_inspector_home_command(const std::string& command)
{
    return command == "plugin_permissions" || command == "plugin_configure" || command == "plugin_config_get" ||
           command == "plugin_config_save" || command == "plugin_config_restore" || command == "plugin_permissions_mutate";
}

}} // namespace Slic3r::GUI
